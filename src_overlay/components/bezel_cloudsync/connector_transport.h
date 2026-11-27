// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_BEZEL_CLOUDSYNC_CONNECTOR_TRANSPORT_H_
#define COMPONENTS_BEZEL_CLOUDSYNC_CONNECTOR_TRANSPORT_H_

#include <memory>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "components/bezel_cloudsync/status.h"

namespace bezel::cloudsync {

// Carries serialized Connector messages. Desktops: gRPC over h2c
// (GrpcTransport). Android: the bound service (`ICloudSyncConnector.call`,
// which already speaks in serialized messages and CallReply codes).
//
// Every method is called on the sequence that created the transport, and
// every callback runs there. Callbacks never run synchronously.
class ConnectorTransport {
 public:
  using UnaryCallback =
      base::OnceCallback<void(Status status, std::string reply)>;

  class WatchObserver {
   public:
    // A serialized `Change`.
    virtual void OnWatchMessage(std::string change) = 0;
    // The stream ended; no more calls after this one.
    virtual void OnWatchEnd(Status status) = 0;

   protected:
    virtual ~WatchObserver() = default;
  };

  // Destroying the handle stops the watch without calling OnWatchEnd.
  class WatchHandle {
   public:
    virtual ~WatchHandle() = default;
  };

  virtual ~ConnectorTransport() = default;

  // `method` is the bare name, e.g. "Hello". `timeout` bounds the whole call;
  // when it passes, the callback gets kDeadlineExceeded.
  virtual void Call(std::string_view method,
                    std::string request,
                    base::TimeDelta timeout,
                    UnaryCallback callback) = 0;

  // `observer` must outlive the handle.
  virtual std::unique_ptr<WatchHandle> Watch(WatchObserver* observer) = 0;

  // Desktop only: the bearer token from pairing, sent with every call; empty
  // clears it. Android identifies the caller through Binder and ignores it.
  virtual void SetToken(std::string token) = 0;
};

}  // namespace bezel::cloudsync

#endif  // COMPONENTS_BEZEL_CLOUDSYNC_CONNECTOR_TRANSPORT_H_
