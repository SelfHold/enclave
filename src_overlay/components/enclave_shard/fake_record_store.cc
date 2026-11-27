// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/fake_record_store.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "components/enclave_shard/status.h"

namespace enclave::shard {

namespace pb = ::selfhold::shard::connector::v1;

class FakeRecordStore::Transport : public ConnectorTransport {
 public:
  Transport(FakeRecordStore* store, std::string device)
      : store_(store), device_(std::move(device)) {
    store_->transports_.insert(this);
  }
  ~Transport() override { store_->transports_.erase(this); }

  void Call(std::string_view method,
            std::string request,
            base::TimeDelta timeout,
            UnaryCallback callback) override {
    std::string reply;
    const int code = store_->Handle(device_, token_, std::string(method),
                                    request, &reply);
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), Status::FromWire(code, ""),
                       code == 0 ? std::move(reply) : std::string()));
  }

  class Handle : public WatchHandle {
   public:
    Handle(base::WeakPtr<Transport> transport, WatchObserver* observer)
        : transport_(std::move(transport)), observer_(observer) {}
    ~Handle() override {
      if (transport_) {
        std::erase(transport_->watchers_, observer_);
      }
    }

   private:
    base::WeakPtr<Transport> transport_;
    raw_ptr<WatchObserver> observer_;
  };

  std::unique_ptr<WatchHandle> Watch(WatchObserver* observer) override {
    watchers_.push_back(observer);
    return std::make_unique<Handle>(weak_factory_.GetWeakPtr(), observer);
  }

  void SetToken(std::string token) override { token_ = std::move(token); }

  void Deliver(const std::string& change) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<Transport> self, std::string change) {
                         if (!self) {
                           return;
                         }
                         std::vector<raw_ptr<WatchObserver>> watchers =
                             self->watchers_;
                         for (WatchObserver* watcher : watchers) {
                           watcher->OnWatchMessage(change);
                         }
                       },
                       weak_factory_.GetWeakPtr(), change));
  }

  const std::string& device() const { return device_; }

 private:
  raw_ptr<FakeRecordStore> store_;
  const std::string device_;
  std::string token_;
  std::vector<raw_ptr<WatchObserver>> watchers_;
  base::WeakPtrFactory<Transport> weak_factory_{this};
};

FakeRecordStore::FakeRecordStore() = default;
FakeRecordStore::~FakeRecordStore() = default;

std::unique_ptr<ConnectorTransport> FakeRecordStore::CreateTransport(
    std::string device) {
  return std::make_unique<Transport>(this, std::move(device));
}

void FakeRecordStore::AddConcurrentVersion(const std::string& key,
                                           std::string data,
                                           std::string device) {
  pb::Item& item = items_[key];
  item.set_key(key);
  item.set_revision("r" + base::NumberToString(next_revision_++));
  pb::Version* version = item.add_versions();
  version->set_id("v" + base::NumberToString(next_revision_++));
  version->set_size(data.size());
  version->set_data(std::move(data));
  version->set_device_name(device);
  NotifyRecord("", key, item.revision(), false);
}

int FakeRecordStore::Handle(const std::string& device,
                            const std::string& token,
                            const std::string& method,
                            const std::string& request,
                            std::string* reply) {
  if (method == "Hello") {
    pb::HelloReply hello;
    hello.set_api_version(1);
    hello.set_state(pb::READY);
    hello.set_paired(required_token_.empty() || token == required_token_);
    hello.add_scopes("passwords");
    hello.set_vault_id("vault");
    *reply = hello.SerializeAsString();
    return 0;
  }
  if (method == "RequestPairing") {
    pb::PairingTicket ticket;
    ticket.set_ticket("t");
    ticket.set_code("123456");
    *reply = ticket.SerializeAsString();
    return 0;
  }
  if (method == "AwaitPairing") {
    pb::PairingResult result;
    result.set_status(pb::APPROVED);
    result.set_token(required_token_);
    result.add_scopes("passwords");
    *reply = result.SerializeAsString();
    return 0;
  }
  if (method == "AppKey") {
    pb::AppKeyReply key;
    key.set_key(std::string(32, 'k'));
    key.set_key_id("kid");
    *reply = key.SerializeAsString();
    return 0;
  }
  if (method == "List") {
    pb::ListRequest list;
    list.ParseFromString(request);
    pb::ListReply answer;
    for (const auto& [key, item] : items_) {
      if (item.versions_size() == 0 || !key.starts_with(list.prefix())) {
        continue;
      }
      pb::Item* listed = answer.add_items();
      listed->set_key(key);
      listed->set_revision(item.revision());
      for (const pb::Version& version : item.versions()) {
        pb::Version* v = listed->add_versions();
        v->set_id(version.id());
        v->set_size(version.size());
      }
    }
    *reply = answer.SerializeAsString();
    return 0;
  }
  if (method == "Get") {
    pb::GetRequest get;
    get.ParseFromString(request);
    auto it = items_.find(get.key());
    pb::Item item;
    item.set_key(get.key());
    if (it != items_.end()) {
      item = it->second;
    }
    *reply = item.SerializeAsString();
    return 0;
  }
  if (method == "Put" || method == "Delete") {
    std::string key, base, data;
    if (method == "Put") {
      pb::PutRequest put;
      put.ParseFromString(request);
      key = put.key();
      base = put.base();
      data = put.data();
      if (fail_puts_) {
        return fail_puts_;
      }
    } else {
      pb::DeleteRequest del;
      del.ParseFromString(request);
      key = del.key();
      base = del.base();
    }
    pb::Item& item = items_[key];
    item.set_key(key);
    if (item.revision() != base) {
      return static_cast<int>(Code::kAborted);
    }
    ++put_count_;
    item.clear_versions();
    item.set_revision("r" + base::NumberToString(next_revision_++));
    if (method == "Put") {
      pb::Version* version = item.add_versions();
      version->set_id("v" + base::NumberToString(next_revision_++));
      version->set_size(data.size());
      version->set_data(std::move(data));
      version->set_device_name(device);
    }
    *reply = item.SerializeAsString();
    NotifyRecord(device, key, item.revision(), method == "Delete");
    return 0;
  }
  return static_cast<int>(Code::kUnimplemented);
}

void FakeRecordStore::NotifyRecord(const std::string& writer,
                                   const std::string& key,
                                   const std::string& revision,
                                   bool deleted) {
  pb::Change change;
  change.set_kind(pb::Change::RECORD);
  change.set_key(key);
  change.set_revision(revision);
  change.set_deleted(deleted);
  const std::string bytes = change.SerializeAsString();
  for (Transport* transport : transports_) {
    // Shard tells every app connection, the writer included; the
    // replicator must skip its own revisions.
    transport->Deliver(bytes);
  }
}

}  // namespace enclave::shard
