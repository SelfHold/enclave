// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_BEZEL_CLOUDSYNC_FAKE_RECORD_STORE_H_
#define COMPONENTS_BEZEL_CLOUDSYNC_FAKE_RECORD_STORE_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/bezel_cloudsync/connector_transport.h"
#include "components/bezel_cloudsync/proto/connector.pb.h"

namespace bezel::cloudsync {

// The record part of the Connector API in memory, shared by several
// "devices" (tests only): Get/List/Put/Delete with revisions and CAS, AppKey,
// Hello, and Watch RECORD changes to every device but the writer. Offline
// devices' writes can be merged into several versions with
// AddConcurrentVersion.
class FakeRecordStore {
 public:
  FakeRecordStore();
  ~FakeRecordStore();

  // A transport for one device; the store must outlive it.
  std::unique_ptr<ConnectorTransport> CreateTransport(std::string device);

  // Adds a version next to the current ones, as a device that wrote from
  // the same base while offline would.
  void AddConcurrentVersion(const std::string& key,
                            std::string data,
                            std::string device);

  const std::map<std::string, ::cloudsync::connector::v1::Item>& items()
      const {
    return items_;
  }
  size_t put_count() const { return put_count_; }
  // Puts fail with this code while set (0 = succeed).
  void set_fail_puts(int code) { fail_puts_ = code; }
  // Hello answers `paired` only for this token, and pairing hands it out
  // (RequestPairing / AwaitPairing approve at once). "" = always paired.
  void set_required_token(std::string token) {
    required_token_ = std::move(token);
  }

 private:
  class Transport;
  friend class Transport;

  int Handle(const std::string& device,
             const std::string& token,
             const std::string& method,
             const std::string& request,
             std::string* reply);
  void NotifyRecord(const std::string& writer,
                    const std::string& key,
                    const std::string& revision,
                    bool deleted);

  std::map<std::string, ::cloudsync::connector::v1::Item> items_;
  int next_revision_ = 1;
  size_t put_count_ = 0;
  int fail_puts_ = 0;
  std::string required_token_;
  std::set<raw_ptr<Transport>> transports_;
};

}  // namespace bezel::cloudsync

#endif  // COMPONENTS_BEZEL_CLOUDSYNC_FAKE_RECORD_STORE_H_
