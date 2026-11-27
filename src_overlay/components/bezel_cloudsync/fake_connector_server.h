// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_BEZEL_CLOUDSYNC_FAKE_CONNECTOR_SERVER_H_
#define COMPONENTS_BEZEL_CLOUDSYNC_FAKE_CONNECTOR_SERVER_H_

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "components/bezel_cloudsync/connector_address.h"
#include "components/bezel_cloudsync/h2c_connection.h"
#include "components/bezel_cloudsync/status.h"

namespace net {
class StreamSocket;
class UnixDomainServerSocket;
}  // namespace net

namespace bezel::cloudsync {

// A gRPC server on a unix socket that answers Connector methods with canned
// handlers. Tests only; needs a sequence with an IO pump.
class FakeConnectorServer : public H2cConnection::ServerHandler {
 public:
  // Fills `reply` (a serialized message) and returns the status.
  using Handler = base::RepeatingCallback<
      Status(const std::string& request, std::string* reply)>;

  struct Request {
    std::string method;
    std::string authorization;  // "" when absent.
  };

  FakeConnectorServer();
  FakeConnectorServer(const FakeConnectorServer&) = delete;
  FakeConnectorServer& operator=(const FakeConnectorServer&) = delete;
  ~FakeConnectorServer() override;

  // Listens on `socket_path`. Returns false on failure.
  bool Start(const std::string& socket_path);
  ConnectorAddress address() const;

  void SetHandler(const std::string& method, Handler handler);
  // A method without a handler that is not held answers UNIMPLEMENTED.
  // Held methods get no answer until Release (to test timeouts).
  void Hold(const std::string& method) { held_.push_back(method); }

  // Sends a serialized Change to every open Watch.
  void PushChange(const std::string& change);
  // Ends every open Watch with `status`.
  void EndWatches(const Status& status);
  size_t open_watches() const { return watches_.size(); }

  // Closes every connection, as if the daemon stopped.
  void DropConnections();

  const std::vector<Request>& requests() const { return requests_; }

  // H2cConnection::ServerHandler:
  void OnRequest(int32_t stream_id,
                 const std::string& path,
                 const H2cConnection::Metadata& metadata,
                 std::string message) override;
  void OnStreamGone(int32_t stream_id) override;

 private:
  void Accept();
  void OnAccepted(int result);

  std::string socket_path_;
  std::unique_ptr<net::UnixDomainServerSocket> listener_;
  std::unique_ptr<net::StreamSocket> accepted_;
  // Only one client connection is ever open in these tests; a new one
  // replaces the old.
  std::unique_ptr<H2cConnection> connection_;
  std::map<std::string, Handler> handlers_;
  std::vector<std::string> held_;
  std::vector<int32_t> watches_;
  std::vector<Request> requests_;
};

}  // namespace bezel::cloudsync

#endif  // COMPONENTS_BEZEL_CLOUDSYNC_FAKE_CONNECTOR_SERVER_H_
