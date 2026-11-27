// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/grpc_transport.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "net/base/net_errors.h"

namespace enclave::shard {

namespace {

constexpr char kServicePath[] = "/selfhold.shard.connector.v1.Connector/";

// Calls that work before pairing and never carry the token (§ 3.1). Hello
// does carry it: Shard answers `paired` from it.
bool SendsToken(std::string_view method) {
  return method != "RequestPairing" && method != "AwaitPairing";
}

}  // namespace

class GrpcTransport::WatchHandleImpl : public WatchHandle {
 public:
  WatchHandleImpl(base::WeakPtr<GrpcTransport> transport, int id)
      : transport_(std::move(transport)), id_(id) {}
  ~WatchHandleImpl() override {
    if (transport_) {
      transport_->Cancel(id_);
    }
  }

 private:
  base::WeakPtr<GrpcTransport> transport_;
  const int id_;
};

GrpcTransport::PendingCall::PendingCall() = default;
GrpcTransport::PendingCall::~PendingCall() = default;

GrpcTransport::GrpcTransport(const base::FilePath& connector_json)
    : GrpcTransport(base::BindRepeating(&ReadConnectorJson, connector_json)) {}

GrpcTransport::GrpcTransport(AddressReader address_reader)
    : address_reader_(std::move(address_reader)) {}

GrpcTransport::~GrpcTransport() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void GrpcTransport::Call(std::string_view method,
                         std::string request,
                         base::TimeDelta timeout,
                         UnaryCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto call = std::make_unique<PendingCall>();
  call->method = std::string(method);
  call->request = std::move(request);
  call->callback = std::move(callback);
  Add(std::move(call), timeout);
}

std::unique_ptr<ConnectorTransport::WatchHandle> GrpcTransport::Watch(
    WatchObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto call = std::make_unique<PendingCall>();
  call->method = "Watch";
  call->observer = observer;
  // WatchRequest is empty: its serialization is "".
  const int id = Add(std::move(call), base::TimeDelta());
  return std::make_unique<WatchHandleImpl>(weak_factory_.GetWeakPtr(), id);
}

void GrpcTransport::SetToken(std::string token) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  token_ = std::move(token);
}

int GrpcTransport::Add(std::unique_ptr<PendingCall> call,
                       base::TimeDelta timeout) {
  const int id = next_id_++;
  if (timeout.is_positive()) {
    call->timer = std::make_unique<base::OneShotTimer>();
    call->timer->Start(FROM_HERE, timeout,
                       base::BindOnce(&GrpcTransport::OnTimeout,
                                      base::Unretained(this), id));
  }
  calls_[id] = std::move(call);
  if (connection_ && connection_->is_open()) {
    StartCall(id);
  } else {
    waiting_.push_back(id);
    EnsureConnected();
  }
  return id;
}

void GrpcTransport::EnsureConnected() {
  if (connecting_ || (connection_ && connection_->is_open())) {
    return;
  }
  connecting_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::OnceCallback<std::optional<ConnectorAddress>()>(address_reader_),
      base::BindOnce(&GrpcTransport::OnAddress, weak_factory_.GetWeakPtr()));
}

void GrpcTransport::OnAddress(std::optional<ConnectorAddress> address) {
  if (!address) {
    connecting_ = false;
    FailWaiting(Status::Transport("Shard is not running"));
    return;
  }
  if (address->api_version != kConnectorApiVersion) {
    connecting_ = false;
    // § 3.1: a bigger number is an incompatible API.
    FailWaiting({Code::kUnimplemented,
                 "Shard speaks connector API " +
                     base::NumberToString(address->api_version) +
                     "; this browser speaks 1"});
    return;
  }
  if (connection_) {
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(connection_));
  }
  connection_ = std::make_unique<H2cConnection>();
  connection_->set_lost_callback(
      base::BindOnce(&GrpcTransport::OnLost, base::Unretained(this)));
  connection_->Connect(*address, base::BindOnce(&GrpcTransport::OnConnected,
                                                base::Unretained(this)));
}

void GrpcTransport::OnConnected(int net_error) {
  connecting_ = false;
  if (net_error != net::OK) {
    FailWaiting(Status::Transport("cannot connect to Shard: " +
                                  net::ErrorToShortString(net_error)));
    return;
  }
  std::vector<int> waiting = std::move(waiting_);
  waiting_.clear();
  for (int id : waiting) {
    if (calls_.contains(id)) {
      StartCall(id);
    }
  }
}

void GrpcTransport::OnLost() {
  // Every started call was closed with a transport error just before this.
  stream_to_call_.clear();
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, std::move(connection_));
  if (!waiting_.empty()) {
    EnsureConnected();
  }
}

void GrpcTransport::StartCall(int id) {
  PendingCall& call = *calls_.at(id);
  H2cConnection::Metadata metadata;
  if (!token_.empty() && SendsToken(call.method)) {
    metadata.emplace_back("authorization", "Bearer " + token_);
  }
  const int32_t stream_id = connection_->StartCall(
      kServicePath + call.method, metadata, call.request, this);
  if (stream_id < 0) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&GrpcTransport::Complete,
                                  weak_factory_.GetWeakPtr(), id,
                                  Status::Transport("cannot start a call")));
    return;
  }
  call.stream_id = stream_id;
  stream_to_call_[stream_id] = id;
}

void GrpcTransport::FailWaiting(const Status& status) {
  std::vector<int> waiting = std::move(waiting_);
  waiting_.clear();
  base::WeakPtr<GrpcTransport> self = weak_factory_.GetWeakPtr();
  for (int id : waiting) {
    if (!self) {
      return;
    }
    Complete(id, status);
  }
}

void GrpcTransport::Complete(int id, Status status) {
  auto it = calls_.find(id);
  if (it == calls_.end()) {
    return;
  }
  std::unique_ptr<PendingCall> call = std::move(it->second);
  calls_.erase(it);
  if (call->stream_id >= 0) {
    stream_to_call_.erase(call->stream_id);
  }
  if (call->observer) {
    call->observer->OnWatchEnd(std::move(status));
    return;
  }
  std::string reply;
  if (status.ok()) {
    if (call->reply) {
      reply = std::move(*call->reply);
    } else {
      status = {Code::kInternal, "Shard sent no reply"};
    }
  }
  std::move(call->callback).Run(std::move(status), std::move(reply));
}

void GrpcTransport::OnTimeout(int id) {
  auto it = calls_.find(id);
  if (it == calls_.end()) {
    return;
  }
  if (it->second->stream_id >= 0 && connection_) {
    connection_->CancelCall(it->second->stream_id);
  }
  std::erase(waiting_, id);
  Complete(id, {Code::kDeadlineExceeded, "Shard did not answer in time"});
}

void GrpcTransport::Cancel(int id) {
  auto it = calls_.find(id);
  if (it == calls_.end()) {
    return;
  }
  if (it->second->stream_id >= 0) {
    if (connection_) {
      connection_->CancelCall(it->second->stream_id);
    }
    stream_to_call_.erase(it->second->stream_id);
  }
  std::erase(waiting_, id);
  calls_.erase(it);
}

void GrpcTransport::OnCallMessage(int32_t stream_id, std::string message) {
  auto it = stream_to_call_.find(stream_id);
  if (it == stream_to_call_.end()) {
    return;
  }
  PendingCall& call = *calls_.at(it->second);
  if (call.observer) {
    call.observer->OnWatchMessage(std::move(message));  // May cancel.
    return;
  }
  if (call.reply) {
    // A unary method answers once; more is a broken server.
    const int id = it->second;
    connection_->CancelCall(stream_id);
    Complete(id, {Code::kInternal, "Shard sent two replies"});
    return;
  }
  call.reply = std::move(message);
}

void GrpcTransport::OnCallClosed(int32_t stream_id, Status status) {
  auto it = stream_to_call_.find(stream_id);
  if (it == stream_to_call_.end()) {
    return;
  }
  Complete(it->second, std::move(status));
}

}  // namespace enclave::shard
