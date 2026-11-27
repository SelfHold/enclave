// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/bezel_cloudsync/fake_connector_server.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "net/base/net_errors.h"
#include "net/socket/stream_socket.h"
#include "net/socket/unix_domain_server_socket_posix.h"

namespace bezel::cloudsync {

namespace {

constexpr char kServicePath[] = "/cloudsync.connector.v1.Connector/";

bool AllowAnyone(const net::UnixDomainServerSocket::Credentials&) {
  return true;
}

}  // namespace

FakeConnectorServer::FakeConnectorServer() = default;
FakeConnectorServer::~FakeConnectorServer() = default;

bool FakeConnectorServer::Start(const std::string& socket_path) {
  socket_path_ = socket_path;
  listener_ = std::make_unique<net::UnixDomainServerSocket>(
      base::BindRepeating(&AllowAnyone), /*use_abstract_namespace=*/false);
  if (listener_->BindAndListen(socket_path, /*backlog=*/4) != net::OK) {
    return false;
  }
  Accept();
  return true;
}

ConnectorAddress FakeConnectorServer::address() const {
  ConnectorAddress address;
  address.kind = ConnectorAddress::Kind::kUnix;
  address.unix_path = socket_path_;
  address.api_version = kConnectorApiVersion;
  return address;
}

void FakeConnectorServer::SetHandler(const std::string& method,
                                     Handler handler) {
  handlers_[method] = std::move(handler);
}

void FakeConnectorServer::Accept() {
  for (;;) {
    const int result = listener_->Accept(
        &accepted_, base::BindOnce(&FakeConnectorServer::OnAccepted,
                                   base::Unretained(this)));
    if (result == net::ERR_IO_PENDING) {
      return;
    }
    if (result != net::OK) {
      return;
    }
    watches_.clear();
    connection_ = std::make_unique<H2cConnection>(std::move(accepted_), this);
  }
}

void FakeConnectorServer::OnAccepted(int result) {
  if (result != net::OK) {
    return;
  }
  watches_.clear();
  connection_ = std::make_unique<H2cConnection>(std::move(accepted_), this);
  Accept();
}

void FakeConnectorServer::OnRequest(int32_t stream_id,
                                    const std::string& path,
                                    const H2cConnection::Metadata& metadata,
                                    std::string message) {
  Request request;
  if (path.starts_with(kServicePath)) {
    request.method = path.substr(sizeof(kServicePath) - 1);
  }
  for (const auto& [key, value] : metadata) {
    if (key == "authorization") {
      request.authorization = value;
    }
  }
  requests_.push_back(request);

  if (request.method == "Watch") {
    watches_.push_back(stream_id);
    return;
  }
  if (std::ranges::contains(held_, request.method)) {
    return;
  }
  auto it = handlers_.find(request.method);
  if (it == handlers_.end()) {
    connection_->Finish(stream_id,
                        {Code::kUnimplemented, "no handler: " + path});
    return;
  }
  std::string reply;
  const Status status = it->second.Run(message, &reply);
  if (status.ok()) {
    connection_->SendMessage(stream_id, reply);
  }
  connection_->Finish(stream_id, status);
}

void FakeConnectorServer::OnStreamGone(int32_t stream_id) {
  std::erase(watches_, stream_id);
}

void FakeConnectorServer::PushChange(const std::string& change) {
  for (int32_t stream_id : watches_) {
    connection_->SendMessage(stream_id, change);
  }
}

void FakeConnectorServer::EndWatches(const Status& status) {
  std::vector<int32_t> watches = std::move(watches_);
  watches_.clear();
  for (int32_t stream_id : watches) {
    connection_->Finish(stream_id, status);
  }
}

void FakeConnectorServer::DropConnections() {
  watches_.clear();
  connection_.reset();
}

}  // namespace bezel::cloudsync
