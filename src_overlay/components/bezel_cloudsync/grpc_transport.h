// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_BEZEL_CLOUDSYNC_GRPC_TRANSPORT_H_
#define COMPONENTS_BEZEL_CLOUDSYNC_GRPC_TRANSPORT_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "components/bezel_cloudsync/connector_address.h"
#include "components/bezel_cloudsync/connector_transport.h"
#include "components/bezel_cloudsync/h2c_connection.h"

namespace bezel::cloudsync {

// Desktop transport: gRPC over h2c to cloudsyncd (CONNECTOR.md § 3.1).
//
// Connects lazily on the first call and again after the connection is lost:
// connector.json is read anew every time, since the daemon writes it only
// while it runs. No retry of its own: a call that cannot reach CloudSync fails
// with a transport Status, and the owner decides when to look again.
//
// Needs a sequence with an IO message pump.
class GrpcTransport : public ConnectorTransport,
                      public H2cConnection::CallObserver {
 public:
  // Runs on the thread pool (may block); returns where the daemon listens.
  using AddressReader =
      base::RepeatingCallback<std::optional<ConnectorAddress>()>;

  // Reads `connector_json` (normally GetConnectorJsonPath()).
  explicit GrpcTransport(const base::FilePath& connector_json);
  explicit GrpcTransport(AddressReader address_reader);
  GrpcTransport(const GrpcTransport&) = delete;
  GrpcTransport& operator=(const GrpcTransport&) = delete;
  ~GrpcTransport() override;

  // ConnectorTransport:
  void Call(std::string_view method,
            std::string request,
            base::TimeDelta timeout,
            UnaryCallback callback) override;
  std::unique_ptr<WatchHandle> Watch(WatchObserver* observer) override;
  void SetToken(std::string token) override;

 private:
  class WatchHandleImpl;

  struct PendingCall {
    PendingCall();
    ~PendingCall();

    std::string method;
    std::string request;
    UnaryCallback callback;                    // Unary.
    raw_ptr<WatchObserver> observer = nullptr;  // Watch.
    std::optional<std::string> reply;
    int32_t stream_id = -1;
    std::unique_ptr<base::OneShotTimer> timer;
  };

  int Add(std::unique_ptr<PendingCall> call, base::TimeDelta timeout);
  void EnsureConnected();
  void OnAddress(std::optional<ConnectorAddress> address);
  void OnConnected(int net_error);
  void OnLost();
  void StartCall(int id);
  void FailWaiting(const Status& status);
  // Removes the call and tells its owner, unless it was cancelled.
  void Complete(int id, Status status);
  void OnTimeout(int id);
  void Cancel(int id);

  // H2cConnection::CallObserver:
  void OnCallMessage(int32_t stream_id, std::string message) override;
  void OnCallClosed(int32_t stream_id, Status status) override;

  const AddressReader address_reader_;
  std::string token_;
  std::unique_ptr<H2cConnection> connection_;
  bool connecting_ = false;

  int next_id_ = 1;
  std::map<int, std::unique_ptr<PendingCall>> calls_;
  std::vector<int> waiting_;               // Calls waiting for a connection.
  std::map<int32_t, int> stream_to_call_;  // Started calls.

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<GrpcTransport> weak_factory_{this};
};

}  // namespace bezel::cloudsync

#endif  // COMPONENTS_BEZEL_CLOUDSYNC_GRPC_TRANSPORT_H_
