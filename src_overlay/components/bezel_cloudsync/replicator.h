// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_BEZEL_CLOUDSYNC_REPLICATOR_H_
#define COMPONENTS_BEZEL_CLOUDSYNC_REPLICATOR_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/clock.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/bezel_cloudsync/connector_client.h"
#include "components/bezel_cloudsync/loopback_bridge.h"
#include "components/bezel_cloudsync/proto/bezel_records.pb.h"
#include "components/sync/base/data_type.h"

namespace bezel::cloudsync {

// Copies the entities of the local sync server (LoopbackServer) to CloudSync
// records and back. See docs/CLOUDSYNC.md, "Replicating LoopbackServer".
//
// Records (all under the profile's prefix "p/<slot>/"):
//   "meta"            shard counts per type (Meta)
//   "t/<field>/<n>"   entities of one type, n = hash(id) % shards(field)
//   "h/w<week>"       HISTORY visits of one week (weeks since 1970)
//   "s/<hash>"        SESSIONS of one device
// Values are gzip-compressed Shard messages.
//
// Two versions of one entity: the larger (changed_ms, device) wins.
// Lives on the UI thread, like ConnectorClient.
class Replicator : public LoopbackListener {
 public:
  using RefreshCallback = base::RepeatingCallback<void(syncer::DataTypeSet)>;

  struct Stats {
    size_t records = 0;          // This profile's.
    size_t other_records = 0;    // Other profiles' and "profiles".
    size_t bytes = 0;
    base::Time last_write;
    // Last problem that stops replication ("" = none).
    std::string error;
  };

  // How long a change waits before its record is written. Every write stays
  // in CloudSync's journals forever, so records are rewritten rarely.
  static constexpr base::TimeDelta kEntityDelay = base::Seconds(60);
  static constexpr base::TimeDelta kSessionsDelay = base::Minutes(15);
  static constexpr base::TimeDelta kHistoryDelay = base::Hours(4);
  // Weeks of history kept in CloudSync (local history is not touched).
  static constexpr int kHistoryWeeks = 12;
  // Tombstones older than this are dropped from records.
  static constexpr base::TimeDelta kTombstoneLifetime = base::Days(90);
  // A shard bigger than this (compressed) is split; CloudSync's limit is
  // 256 KiB per record.
  static constexpr size_t kMaxShardBytes = 192 * 1024;
  static constexpr size_t kMaxRecordBytes = 250 * 1024;
  // Of CloudSync's 256 records per app; the rest is spare.
  static constexpr size_t kMaxRecords = 248;

  // `client` must outlive this. `backend_file` is the LoopbackServer's file
  // (the bridge key); the index is kept next to it. `device_id` names this
  // device in entries. Every record key gets `key_prefix` ("p/<slot>/", one
  // per browser profile); records of other prefixes are only counted.
  Replicator(ConnectorClient* client,
             const base::FilePath& backend_file,
             std::string device_id,
             std::string key_prefix,
             RefreshCallback refresh,
             const base::Clock* clock = nullptr);
  Replicator(const Replicator&) = delete;
  Replicator& operator=(const Replicator&) = delete;
  ~Replicator() override;

  // Starts replicating once the replica is there. If the server's keystore
  // key is not `keystore_key`, the server is reset with it first (pairing,
  // or another vault).
  void Start(std::vector<uint8_t> keystore_key);
  void Stop();
  bool running() const { return running_; }

  // From the connector's Watch (RECORD; the full key) and state changes.
  void OnRecordChanged(const std::string& full_key,
                       const std::string& revision,
                       bool deleted);
  // Writes became possible again (state READY): retry what is pending.
  void OnWritable();
  // Writes everything that is pending now (e.g. before switching off).
  void FlushNow();

  // Deletes every record of this profile ("Remove synced data").
  void RemoveAllRecords(base::OnceCallback<void(Status)> done);

  const Stats& stats() const { return stats_; }

  // Record key of an entity, or nullopt if it is not replicated. Public for
  // tests.
  std::optional<std::string> KeyFor(const records::Entry& entry) const;
  static bool IsReplicated(const sync_pb::LoopbackServerEntity& entity);
  static syncer::DataType TypeOf(const sync_pb::LoopbackServerEntity& entity);

  // LoopbackListener:
  void OnReplicaAvailable() override;
  void OnLocalCommit(LoopbackReplica::Entities entities) override;
  void OnReplicaGone() override;

 private:
  enum class Cadence { kEntity, kSessions, kHistory };

  struct Record {
    Record();
    Record(const Record&);
    ~Record();

    std::string revision;  // "" = never written.
    bool exists = false;
    bool dirty = false;
    bool in_flight = false;
    bool dirty_again = false;  // Changed while a write was in flight.
    std::string written_hash;  // Of the last value written or read.
    size_t size = 0;
  };

  // Startup: read the index, list and read every record, then reconcile.
  void ReadIndex();
  void OnIndexRead(std::optional<records::ReplicaIndex> index);
  void ListRemote();
  void OnListed(Status status, pb::ListReply reply);
  void OnRemoteRead(const std::string& key, Status status, pb::Item item);
  void ReconcileWithReplica();
  void OnExported(LoopbackReplica::Entities entities);

  // Merges one record into `known_`; returns entries that beat ours.
  std::vector<records::Entry> MergeRemote(const std::string& key,
                                          const pb::Item& item);
  void ImportIntoReplica(std::vector<records::Entry> entries);
  void OnImported(std::vector<int> field_numbers);

  // True if `a` is newer than `b`.
  static bool Newer(const records::Entry& a, const records::Entry& b);
  void Remember(records::Entry entry);
  void MarkDirty(const std::string& key);
  static Cadence CadenceOf(const std::string& key);
  void ScheduleFlush(Cadence cadence);
  void FlushCadence(Cadence cadence);
  // False when the type had to be split (the caller's grouping is stale).
  bool Write(const std::string& key,
             const std::vector<const records::Entry*>& entries);
  void OnWritten(const std::string& key,
                 std::string hash,
                 size_t size,
                 Status status,
                 pb::Item item);
  void OnRereadAfterConflict(const std::string& key,
                             Status status,
                             pb::Item item);
  // Serialized Shard of `entries` (all of `key`); nullopt = nothing to keep.
  std::optional<std::string> BuildValue(
      const std::string& key,
      const std::vector<const records::Entry*>& entries);
  void SplitType(int field_number);
  void WriteMeta();
  void DropOldHistory();
  void UpdateStats();
  void SaveIndexSoon();
  void SaveIndex();
  void Fail(std::string error);

  raw_ptr<ConnectorClient> client_;
  const std::string key_prefix_;
  size_t foreign_records_ = 0;
  const base::FilePath backend_file_;
  const base::FilePath index_file_;
  const std::string device_id_;
  const RefreshCallback refresh_;
  raw_ptr<const base::Clock> clock_;

  bool running_ = false;
  bool replica_available_ = false;
  bool reconciled_ = false;
  std::vector<uint8_t> keystore_key_;
  bool reset_pending_ = false;

  // Every replicated entity this device knows, by id.
  std::map<std::string, records::Entry> known_;
  // Change times from the index file, used for entities without an entry.
  std::map<std::string, records::IndexEntry> index_;
  std::map<std::string, Record> records_;
  std::map<int32_t, int32_t> shards_;  // field number -> shard count.
  std::string meta_revision_;
  bool meta_dirty_ = false;
  bool meta_in_flight_ = false;

  // Startup bookkeeping.
  bool listing_ = false;
  size_t pending_reads_ = 0;
  std::vector<std::string> keys_to_read_;

  std::map<Cadence, std::unique_ptr<base::OneShotTimer>> timers_;
  base::OneShotTimer index_timer_;
  Stats stats_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<Replicator> weak_factory_{this};
};

}  // namespace bezel::cloudsync

#endif  // COMPONENTS_BEZEL_CLOUDSYNC_REPLICATOR_H_
