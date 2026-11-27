// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/connector_client.h"

#include <algorithm>

#include "base/memory/raw_ptr.h"

namespace enclave::shard {

namespace {

// Parses Change messages for a typed observer. Owns the transport's handle,
// so the transport stops calling before this object goes away.
class TypedWatch : public ConnectorTransport::WatchHandle,
                   public ConnectorTransport::WatchObserver {
 public:
  explicit TypedWatch(ConnectorClient::WatchObserver* observer)
      : observer_(observer) {}
  ~TypedWatch() override { inner_.reset(); }

  void set_inner(std::unique_ptr<ConnectorTransport::WatchHandle> inner) {
    inner_ = std::move(inner);
  }

  void OnWatchMessage(std::string bytes) override {
    pb::Change change;
    if (!change.ParseFromString(bytes)) {
      // Unknown fields parse fine (proto3), so this is a broken message.
      // Drop the stream: the owner reconnects and lists again.
      inner_.reset();
      observer_->OnWatchEnd(
          {Code::kInternal, "Shard sent a change that does not parse"});
      return;
    }
    observer_->OnChange(change);
  }

  void OnWatchEnd(Status status) override {
    observer_->OnWatchEnd(std::move(status));
  }

 private:
  raw_ptr<ConnectorClient::WatchObserver> observer_;
  std::unique_ptr<ConnectorTransport::WatchHandle> inner_;
};

}  // namespace

ConnectorClient::ConnectorClient(std::unique_ptr<ConnectorTransport> transport)
    : transport_(std::move(transport)) {}

ConnectorClient::~ConnectorClient() = default;

void ConnectorClient::SetToken(std::string token) {
  transport_->SetToken(std::move(token));
}

void ConnectorClient::Hello(const pb::HelloRequest& request,
                            Callback<pb::HelloReply> cb) {
  Call("Hello", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::RequestPairing(const pb::PairingRequest& request,
                                     Callback<pb::PairingTicket> cb) {
  Call("RequestPairing", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::AwaitPairing(const pb::AwaitPairingRequest& request,
                                   Callback<pb::PairingResult> cb) {
  // Shard waits at most 60 s (§ 5).
  const base::TimeDelta wait =
      base::Seconds(std::min<uint32_t>(request.wait_seconds(), 60));
  Call("AwaitPairing", request, kCallTimeout + wait, std::move(cb));
}

void ConnectorClient::Get(const pb::GetRequest& request,
                          Callback<pb::Item> cb) {
  Call("Get", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::List(const pb::ListRequest& request,
                           Callback<pb::ListReply> cb) {
  Call("List", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::Put(const pb::PutRequest& request,
                          Callback<pb::Item> cb) {
  Call("Put", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::Delete(const pb::DeleteRequest& request,
                             Callback<pb::Item> cb) {
  Call("Delete", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::AppKey(Callback<pb::AppKeyReply> cb) {
  Call("AppKey", pb::AppKeyRequest(), kCallTimeout, std::move(cb));
}

void ConnectorClient::Unpair(Callback<pb::UnpairReply> cb) {
  Call("Unpair", pb::UnpairRequest(), kCallTimeout, std::move(cb));
}

void ConnectorClient::ListLogins(const pb::ListLoginsRequest& request,
                                 Callback<pb::ListLoginsReply> cb) {
  Call("ListLogins", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::GetLogins(const pb::GetLoginsRequest& request,
                                Callback<pb::ListLoginsReply> cb) {
  Call("GetLogins", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::PutLogin(const pb::PutLoginRequest& request,
                               Callback<pb::Login> cb) {
  Call("PutLogin", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::DeleteLogin(const pb::DeleteLoginRequest& request,
                                  Callback<pb::DeleteLoginReply> cb) {
  Call("DeleteLogin", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::TouchLogin(const pb::TouchLoginRequest& request,
                                 Callback<pb::TouchLoginReply> cb) {
  Call("TouchLogin", request, kCallTimeout, std::move(cb));
}

void ConnectorClient::ListChanges(const pb::ListChangesRequest& request,
                                  Callback<pb::ListChangesReply> cb) {
  Call("ListChanges", request, kCallTimeout, std::move(cb));
}

std::unique_ptr<ConnectorTransport::WatchHandle> ConnectorClient::Watch(
    WatchObserver* observer) {
  auto watch = std::make_unique<TypedWatch>(observer);
  watch->set_inner(transport_->Watch(watch.get()));
  return watch;
}

}  // namespace enclave::shard
