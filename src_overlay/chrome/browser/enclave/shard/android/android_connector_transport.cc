// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/enclave/shard/android/android_connector_transport.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "components/enclave_shard/proto/connector.pb.h"
#include "components/enclave_shard/status.h"
#include "third_party/jni_zero/jni_zero.h"

// Must come after the headers that define the @JniType conversions.
#include "chrome/browser/enclave/shard/android/jni/ShardConnectorBridge_jni.h"

namespace enclave {

namespace pb = ::selfhold::shard::connector::v1;
using shard::ConnectorTransport;
using shard::Status;

// Not in an anonymous namespace: the JNI glue (DEFINE_JNI below) names it as
// enclave::AndroidConnectorTransport, from `nativeAndroidConnectorTransport`.
class AndroidConnectorTransport : public ConnectorTransport {
 public:
  AndroidConnectorTransport() {
    JNIEnv* env = base::android::AttachCurrentThread();
    java_bridge_.Reset(Java_ShardConnectorBridge_create(
        env, reinterpret_cast<int64_t>(this)));
  }

  AndroidConnectorTransport(const AndroidConnectorTransport&) = delete;
  AndroidConnectorTransport& operator=(const AndroidConnectorTransport&) =
      delete;

  ~AndroidConnectorTransport() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    // Pending callbacks are dropped, as on desktop when the transport goes.
    Java_ShardConnectorBridge_destroy(base::android::AttachCurrentThread(),
                                          java_bridge_);
  }

  // ConnectorTransport:
  void Call(std::string_view method,
            std::string request,
            base::TimeDelta timeout,
            UnaryCallback callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    const int32_t id = next_id_++;
    PendingCall& call = calls_[id];
    call.callback = std::move(callback);
    if (timeout.is_positive()) {
      call.timer = std::make_unique<base::OneShotTimer>();
      call.timer->Start(
          FROM_HERE, timeout,
          base::BindOnce(&AndroidConnectorTransport::OnTimeout,
                         weak_factory_.GetWeakPtr(), id));
    }
    // Java answers on a later UI task (the binder call runs on a background
    // executor), so the callback never runs synchronously.
    JNIEnv* env = base::android::AttachCurrentThread();
    Java_ShardConnectorBridge_call(
        env, java_bridge_, id, std::string(method),
        std::vector<uint8_t>(request.begin(), request.end()),
        timeout.is_positive() ? timeout.InMilliseconds() : int64_t{0});
  }

  std::unique_ptr<WatchHandle> Watch(WatchObserver* observer) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    const int32_t id = next_id_++;
    watches_[id] = observer;
    Java_ShardConnectorBridge_watch(base::android::AttachCurrentThread(),
                                        java_bridge_, id);
    return std::make_unique<WatchHandleImpl>(weak_factory_.GetWeakPtr(), id);
  }

  // Binder identifies the caller (package + signing certificate).
  void SetToken(std::string token) override {}

  // Called from Java, on the UI thread.
  void OnCallReply(JNIEnv* env,
                   int32_t id,
                   bool transport_error,
                   const std::vector<uint8_t>& reply,
                   const std::string& error) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    auto it = calls_.find(id);
    if (it == calls_.end()) {
      return;  // Timed out already: the late reply is dropped.
    }
    UnaryCallback callback = std::move(it->second.callback);
    calls_.erase(it);
    if (transport_error) {
      std::move(callback).Run(Status::Transport(error), std::string());
      return;
    }
    pb::CallReply call_reply;
    if (!call_reply.ParseFromArray(reply.data(),
                                   static_cast<int>(reply.size()))) {
      std::move(callback).Run(
          Status::FromWire(static_cast<int>(shard::Code::kInternal),
                           "Malformed CallReply from Shard"),
          std::string());
      return;
    }
    if (call_reply.code() != 0) {
      std::move(callback).Run(
          Status::FromWire(call_reply.code(), call_reply.message()),
          std::string());
      return;
    }
    std::move(callback).Run(Status::Ok(), call_reply.payload());
  }

  void OnWatchChange(JNIEnv* env,
                     int32_t watch_id,
                     const std::vector<uint8_t>& change) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    auto it = watches_.find(watch_id);
    if (it == watches_.end()) {
      return;
    }
    it->second->OnWatchMessage(std::string(change.begin(), change.end()));
  }

  void OnWatchEnd(JNIEnv* env,
                  int32_t watch_id,
                  bool transport_error,
                  const std::vector<uint8_t>& reply,
                  const std::string& error) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    auto it = watches_.find(watch_id);
    if (it == watches_.end()) {
      return;
    }
    WatchObserver* observer = it->second;
    watches_.erase(it);
    Status status;
    if (transport_error) {
      status = Status::Transport(error);
    } else {
      pb::CallReply call_reply;
      if (call_reply.ParseFromArray(reply.data(),
                                    static_cast<int>(reply.size()))) {
        status = Status::FromWire(call_reply.code(), call_reply.message());
      } else {
        status = Status::FromWire(static_cast<int>(shard::Code::kInternal),
                                  "Malformed CallReply from Shard");
      }
    }
    observer->OnWatchEnd(std::move(status));
  }

 private:
  struct PendingCall {
    UnaryCallback callback;
    std::unique_ptr<base::OneShotTimer> timer;
  };

  class WatchHandleImpl : public WatchHandle {
   public:
    WatchHandleImpl(base::WeakPtr<AndroidConnectorTransport> transport,
                    int32_t id)
        : transport_(std::move(transport)), id_(id) {}
    ~WatchHandleImpl() override {
      if (transport_) {
        transport_->Unwatch(id_);
      }
    }

   private:
    base::WeakPtr<AndroidConnectorTransport> transport_;
    const int32_t id_;
  };

  void Unwatch(int32_t id) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (watches_.erase(id) == 0) {
      return;  // Ended already.
    }
    Java_ShardConnectorBridge_unwatch(base::android::AttachCurrentThread(),
                                          java_bridge_, id);
  }

  void OnTimeout(int32_t id) {
    auto it = calls_.find(id);
    if (it == calls_.end()) {
      return;
    }
    UnaryCallback callback = std::move(it->second.callback);
    // Erasing destroys the timer that is running this task; that is allowed
    // for base::OneShotTimer once its task has started.
    calls_.erase(it);
    std::move(callback).Run(
        Status::FromWire(static_cast<int>(shard::Code::kDeadlineExceeded),
                         "Shard call timed out"),
        std::string());
  }

  SEQUENCE_CHECKER(sequence_checker_);
  base::android::ScopedJavaGlobalRef<jobject> java_bridge_;
  int32_t next_id_ = 1;
  std::map<int32_t, PendingCall> calls_;
  std::map<int32_t, raw_ptr<WatchObserver>> watches_;
  base::WeakPtrFactory<AndroidConnectorTransport> weak_factory_{this};
};

std::unique_ptr<shard::ConnectorTransport>
CreateAndroidConnectorTransport() {
  return std::make_unique<AndroidConnectorTransport>();
}

}  // namespace enclave

DEFINE_JNI(ShardConnectorBridge)
