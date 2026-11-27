// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_BEZEL_CLOUDSYNC_CONNECTOR_CLIENT_H_
#define COMPONENTS_BEZEL_CLOUDSYNC_CONNECTOR_CLIENT_H_

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/time/time.h"
#include "components/bezel_cloudsync/connector_transport.h"
#include "components/bezel_cloudsync/proto/connector.pb.h"
#include "components/bezel_cloudsync/status.h"

namespace bezel::cloudsync {

namespace pb = ::cloudsync::connector::v1;

// The app id of the browser's data in CloudSync. Never change it: another id
// sees other data (CONNECTOR.md § 2). The browser is named "Enclave" in
// CloudSync (renamed from "bezel" on 2026-11-26, before any data was synced).
inline constexpr char kBezelAppId[] = "enclave";

// Typed Connector API v1 over a ConnectorTransport. One object per profile;
// lives on the transport's sequence. Replies come asynchronously; a reply
// that does not parse turns into kInternal.
class ConnectorClient {
 public:
  template <typename Reply>
  using Callback = base::OnceCallback<void(Status, Reply)>;

  class WatchObserver {
   public:
    virtual void OnChange(const pb::Change& change) = 0;
    // The stream ended: reconnect with backoff, Hello, then List (§ 6.3).
    virtual void OnWatchEnd(Status status) = 0;

   protected:
    virtual ~WatchObserver() = default;
  };

  // Deadline of ordinary calls. AwaitPairing gets its wait on top.
  static constexpr base::TimeDelta kCallTimeout = base::Seconds(15);

  explicit ConnectorClient(std::unique_ptr<ConnectorTransport> transport);
  ConnectorClient(const ConnectorClient&) = delete;
  ConnectorClient& operator=(const ConnectorClient&) = delete;
  ~ConnectorClient();

  // Desktop: the bearer token from pairing. Kept on this device only.
  void SetToken(std::string token);

  void Hello(const pb::HelloRequest& request, Callback<pb::HelloReply> cb);
  void RequestPairing(const pb::PairingRequest& request,
                      Callback<pb::PairingTicket> cb);
  void AwaitPairing(const pb::AwaitPairingRequest& request,
                    Callback<pb::PairingResult> cb);

  void Get(const pb::GetRequest& request, Callback<pb::Item> cb);
  void List(const pb::ListRequest& request, Callback<pb::ListReply> cb);
  void Put(const pb::PutRequest& request, Callback<pb::Item> cb);
  void Delete(const pb::DeleteRequest& request, Callback<pb::Item> cb);
  void AppKey(Callback<pb::AppKeyReply> cb);
  void Unpair(Callback<pb::UnpairReply> cb);

  // Scope "passwords" (§ 12).
  void ListLogins(const pb::ListLoginsRequest& request,
                  Callback<pb::ListLoginsReply> cb);
  void GetLogins(const pb::GetLoginsRequest& request,
                 Callback<pb::ListLoginsReply> cb);
  void PutLogin(const pb::PutLoginRequest& request, Callback<pb::Login> cb);
  void DeleteLogin(const pb::DeleteLoginRequest& request,
                   Callback<pb::DeleteLoginReply> cb);
  void TouchLogin(const pb::TouchLoginRequest& request,
                  Callback<pb::TouchLoginReply> cb);
  void ListChanges(const pb::ListChangesRequest& request,
                   Callback<pb::ListChangesReply> cb);

  // Stops when the handle is destroyed. `observer` must outlive the handle.
  std::unique_ptr<ConnectorTransport::WatchHandle> Watch(
      WatchObserver* observer);

 private:
  template <typename Reply>
  void Call(std::string_view method,
            const google::protobuf::MessageLite& request,
            base::TimeDelta timeout,
            Callback<Reply> callback) {
    transport_->Call(method, request.SerializeAsString(), timeout,
                     base::BindOnce(&ConnectorClient::Parse<Reply>,
                                    std::move(callback)));
  }

  template <typename Reply>
  static void Parse(Callback<Reply> callback,
                    Status status,
                    std::string bytes) {
    Reply reply;
    if (status.ok() && !reply.ParseFromString(bytes)) {
      status = {Code::kInternal, "CloudSync sent a reply that does not parse"};
    }
    std::move(callback).Run(std::move(status), std::move(reply));
  }

  std::unique_ptr<ConnectorTransport> transport_;
};

}  // namespace bezel::cloudsync

#endif  // COMPONENTS_BEZEL_CLOUDSYNC_CONNECTOR_CLIENT_H_
