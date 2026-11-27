// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

package org.chromium.chrome.browser.bezel.cloudsync;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.SystemClock;

import dev.cloudsync.connector.ICloudSyncConnector;
import dev.cloudsync.connector.IConnectorWatcher;

import org.jni_zero.CalledByNative;
import org.jni_zero.JNINamespace;
import org.jni_zero.JniType;
import org.jni_zero.NativeMethods;

import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.ThreadUtils;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Bezel: the Connector API over CloudSync's bound service (CONNECTOR.md § 3.2). The native
 * counterpart is AndroidConnectorTransport.
 *
 * <p>Native calls come in on the UI thread. Binder calls block (AwaitPairing up to 60 s), so they
 * run on a background executor; every result goes back to native on the UI thread. Timeouts are
 * enforced by native, which drops late results.
 */
@NullMarked
@JNINamespace("bezel")
class CloudSyncConnectorBridge {
    private static final String TAG = "BezelCloudSync";
    private static final String PACKAGE = "dev.cloudsync.cloudsync";
    private static final String ACTION_BIND = "dev.cloudsync.connector.BIND";
    // Upper bound for waiting on onServiceConnected.
    private static final long BIND_TIMEOUT_MS = 10_000;

    /** One running Watch. */
    private static final class ActiveWatch {
        final IConnectorWatcher.Stub mStub;
        // Set once CloudSync answered watch(); null before that.
        @Nullable Long mHandle;
        // The service that runs it: a watch does not survive a disconnect.
        @Nullable ICloudSyncConnector mService;

        ActiveWatch(IConnectorWatcher.Stub stub) {
            mStub = stub;
        }
    }

    // Accessed on the UI thread only.
    private long mNativeAndroidConnectorTransport;

    // Calls can block for a minute each: a cached pool keeps a long AwaitPairing from
    // holding up Hello or Get. Threads end after 60 s idle.
    private final ExecutorService mExecutor = Executors.newCachedThreadPool();

    private final Object mLock = new Object();
    // Guarded by mLock.
    private @Nullable ICloudSyncConnector mService;
    private @Nullable ServiceConnection mConnection;
    // True from bindService() until onServiceConnected or the binding is dropped.
    private boolean mBinding;
    private boolean mDestroyed;
    private final Map<Integer, ActiveWatch> mWatches = new HashMap<>();

    private CloudSyncConnectorBridge(long nativePtr) {
        mNativeAndroidConnectorTransport = nativePtr;
    }

    @CalledByNative
    static CloudSyncConnectorBridge create(long nativePtr) {
        return new CloudSyncConnectorBridge(nativePtr);
    }

    /** Native is gone: no more callbacks, unbind. */
    @CalledByNative
    void destroy() {
        ThreadUtils.assertOnUiThread();
        mNativeAndroidConnectorTransport = 0;
        List<ActiveWatch> watches;
        synchronized (mLock) {
            mDestroyed = true;
            watches = new ArrayList<>(mWatches.values());
            mWatches.clear();
            mLock.notifyAll();
        }
        mExecutor.execute(
                () -> {
                    for (ActiveWatch watch : watches) {
                        stopRemoteWatch(watch);
                    }
                    synchronized (mLock) {
                        unbindLocked();
                    }
                });
        mExecutor.shutdown();
    }

    /** A unary call; the reply comes back through Natives.onCallReply with the same id. */
    @CalledByNative
    void call(
            int id,
            @JniType("std::string") String method,
            @JniType("std::vector<uint8_t>") byte[] request,
            long timeoutMs) {
        ThreadUtils.assertOnUiThread();
        mExecutor.execute(
                () -> {
                    byte[] reply = null;
                    String error = "";
                    try {
                        ICloudSyncConnector service = awaitService(timeoutMs);
                        reply = service.call(method, request);
                        if (reply == null) error = "CloudSync returned no reply";
                    } catch (TransportException e) {
                        error = e.getMessage();
                    } catch (RemoteException | RuntimeException e) {
                        // DeadObjectException is a RemoteException. A RuntimeException is what
                        // Binder rethrows from the service side (e.g. SecurityException).
                        error = "CloudSync call failed: " + e;
                        onRemoteFailure(e);
                    }
                    final byte[] result = reply;
                    final String message = error == null ? "" : error;
                    ThreadUtils.postOnUiThread(
                            () -> {
                                if (mNativeAndroidConnectorTransport == 0) return;
                                CloudSyncConnectorBridgeJni.get()
                                        .onCallReply(
                                                mNativeAndroidConnectorTransport,
                                                id,
                                                result == null,
                                                result == null ? new byte[0] : result,
                                                message);
                            });
                });
    }

    /**
     * Starts a Watch. Changes come back through Natives.onWatchChange, then exactly one
     * Natives.onWatchEnd, unless native stops it first with unwatch().
     */
    @CalledByNative
    void watch(int watchId) {
        ThreadUtils.assertOnUiThread();
        IConnectorWatcher.Stub stub =
                new IConnectorWatcher.Stub() {
                    @Override
                    public void onChange(byte[] change) {
                        final byte[] bytes = change == null ? new byte[0] : change;
                        ThreadUtils.postOnUiThread(() -> deliverChange(watchId, bytes));
                    }

                    @Override
                    public void onEnd(byte[] reply) {
                        synchronized (mLock) {
                            if (mWatches.remove(watchId) == null) return;
                        }
                        final byte[] bytes = reply == null ? new byte[0] : reply;
                        ThreadUtils.postOnUiThread(
                                () -> deliverEnd(watchId, false, bytes, ""));
                    }
                };
        ActiveWatch watch = new ActiveWatch(stub);
        synchronized (mLock) {
            mWatches.put(watchId, watch);
        }
        mExecutor.execute(
                () -> {
                    String error;
                    try {
                        ICloudSyncConnector service = awaitService(BIND_TIMEOUT_MS);
                        long handle = service.watch(stub);
                        boolean stillWanted;
                        synchronized (mLock) {
                            stillWanted = mWatches.get(watchId) == watch;
                            if (stillWanted) {
                                watch.mHandle = handle;
                                watch.mService = service;
                            }
                        }
                        if (!stillWanted) {
                            // Unwatched (or ended) while watch() was in flight.
                            try {
                                service.unwatch(handle);
                            } catch (RemoteException | RuntimeException e) {
                                // The service is gone: so is the watch.
                            }
                        }
                        return;
                    } catch (TransportException e) {
                        error = e.getMessage();
                    } catch (RemoteException | RuntimeException e) {
                        error = "CloudSync watch failed: " + e;
                        onRemoteFailure(e);
                    }
                    synchronized (mLock) {
                        if (mWatches.get(watchId) != watch) return;
                        mWatches.remove(watchId);
                    }
                    final String message = error == null ? "" : error;
                    ThreadUtils.postOnUiThread(
                            () -> deliverEnd(watchId, true, new byte[0], message));
                });
    }

    /** Stops a Watch without an onWatchEnd. */
    @CalledByNative
    void unwatch(int watchId) {
        ThreadUtils.assertOnUiThread();
        ActiveWatch removed;
        synchronized (mLock) {
            removed = mWatches.remove(watchId);
        }
        if (removed == null) return;
        final ActiveWatch watch = removed;
        mExecutor.execute(() -> stopRemoteWatch(watch));
    }

    // Native drops results for watch ids it no longer knows (after unwatch).
    private void deliverChange(int watchId, byte[] change) {
        if (mNativeAndroidConnectorTransport == 0) return;
        CloudSyncConnectorBridgeJni.get()
                .onWatchChange(mNativeAndroidConnectorTransport, watchId, change);
    }

    private void deliverEnd(int watchId, boolean transportError, byte[] reply, String error) {
        if (mNativeAndroidConnectorTransport == 0) return;
        CloudSyncConnectorBridgeJni.get()
                .onWatchEnd(
                        mNativeAndroidConnectorTransport, watchId, transportError, reply, error);
    }

    private void stopRemoteWatch(ActiveWatch watch) {
        ICloudSyncConnector service;
        Long handle;
        synchronized (mLock) {
            service = watch.mService;
            handle = watch.mHandle;
        }
        if (service == null || handle == null) return;
        try {
            service.unwatch(handle);
        } catch (RemoteException | RuntimeException e) {
            // The service is gone: so is the watch.
        }
    }

    /** Thrown when CloudSync cannot be reached at all. */
    private static final class TransportException extends Exception {
        TransportException(String message) {
            super(message);
        }
    }

    /**
     * Returns the bound service, binding first when needed. Blocks the calling (background)
     * thread until onServiceConnected, at most min(timeoutMs, BIND_TIMEOUT_MS).
     */
    private ICloudSyncConnector awaitService(long timeoutMs) throws TransportException {
        long wait = timeoutMs > 0 ? Math.min(timeoutMs, BIND_TIMEOUT_MS) : BIND_TIMEOUT_MS;
        long deadline = SystemClock.elapsedRealtime() + wait;
        synchronized (mLock) {
            if (mDestroyed) throw new TransportException("CloudSync bridge destroyed");
            if (mService != null) return mService;
            if (!mBinding) bindLocked();
            while (mService == null && mBinding && !mDestroyed) {
                long left = deadline - SystemClock.elapsedRealtime();
                if (left <= 0) break;
                try {
                    mLock.wait(left);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new TransportException("Interrupted while binding CloudSync");
                }
            }
            if (mService != null) return mService;
            if (mDestroyed) throw new TransportException("CloudSync bridge destroyed");
            if (!mBinding) throw new TransportException("CloudSync is not available");
            throw new TransportException("Timed out binding CloudSync");
        }
    }

    // Requires mLock.
    private void bindLocked() throws TransportException {
        Context context = ContextUtils.getApplicationContext();
        Intent intent = new Intent(ACTION_BIND).setPackage(PACKAGE);
        ServiceConnection connection =
                new ServiceConnection() {
                    @Override
                    public void onServiceConnected(ComponentName name, IBinder binder) {
                        synchronized (mLock) {
                            if (mConnection != this) return;
                            mService = ICloudSyncConnector.Stub.asInterface(binder);
                            mBinding = false;
                            mLock.notifyAll();
                        }
                    }

                    @Override
                    public void onServiceDisconnected(ComponentName name) {
                        dropBinding(this, "CloudSync disconnected");
                    }

                    @Override
                    public void onBindingDied(ComponentName name) {
                        dropBinding(this, "CloudSync binding died");
                    }

                    @Override
                    public void onNullBinding(ComponentName name) {
                        dropBinding(this, "CloudSync refused the binding");
                    }
                };
        boolean bound;
        try {
            bound = context.bindService(intent, connection, Context.BIND_AUTO_CREATE);
        } catch (SecurityException e) {
            bound = false;
        }
        if (!bound) {
            // bindService() wants an unbindService() even when it returns false.
            try {
                context.unbindService(connection);
            } catch (IllegalArgumentException e) {
                // Not registered: nothing to undo.
            }
            throw new TransportException("CloudSync is not installed");
        }
        mConnection = connection;
        mBinding = true;
    }

    // Requires mLock.
    private void unbindLocked() {
        ServiceConnection connection = mConnection;
        mConnection = null;
        mService = null;
        mBinding = false;
        mLock.notifyAll();
        if (connection == null) return;
        try {
            ContextUtils.getApplicationContext().unbindService(connection);
        } catch (IllegalArgumentException e) {
            // Already unbound.
        }
    }

    /**
     * The binding is lost: unbind now, bind again on the next call. Running watches die with
     * the service, so each ends with a transport error.
     */
    private void dropBinding(ServiceConnection connection, String reason) {
        List<Integer> ended = new ArrayList<>();
        synchronized (mLock) {
            if (mConnection != connection) return;
            Log.w(TAG, reason);
            unbindLocked();
            ended.addAll(mWatches.keySet());
            mWatches.clear();
        }
        for (int watchId : ended) {
            ThreadUtils.postOnUiThread(() -> deliverEnd(watchId, true, new byte[0], reason));
        }
    }

    /** A Binder call failed: if the service died, forget it so that the next call rebinds. */
    private void onRemoteFailure(Exception e) {
        if (!(e instanceof android.os.DeadObjectException)) return;
        ServiceConnection connection;
        synchronized (mLock) {
            connection = mConnection;
        }
        if (connection != null) dropBinding(connection, "CloudSync died");
    }

    @NativeMethods
    interface Natives {
        void onCallReply(
                long nativeAndroidConnectorTransport,
                int id,
                boolean transportError,
                @JniType("std::vector<uint8_t>") byte[] reply,
                @JniType("std::string") String error);

        void onWatchChange(
                long nativeAndroidConnectorTransport,
                int watchId,
                @JniType("std::vector<uint8_t>") byte[] change);

        void onWatchEnd(
                long nativeAndroidConnectorTransport,
                int watchId,
                boolean transportError,
                @JniType("std::vector<uint8_t>") byte[] reply,
                @JniType("std::string") String error);
    }
}
