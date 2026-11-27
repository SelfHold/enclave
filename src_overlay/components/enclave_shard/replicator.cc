// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/replicator.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/hash/hash.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/default_clock.h"
#include "crypto/hash.h"
#include "third_party/zlib/google/compression_utils.h"

namespace enclave::shard {

namespace {

constexpr char kMetaKey[] = "meta";
constexpr char kTypePrefix[] = "t/";
constexpr char kHistoryPrefix[] = "h/w";
constexpr char kSessionsPrefix[] = "s/";
constexpr int kFormat = 1;
constexpr int kMaxShardsPerType = 64;
constexpr base::FilePath::CharType kIndexFileName[] =
    FILE_PATH_LITERAL("shard_index.pb");

std::string HashOf(const std::string& bytes) {
  return base::HexEncode(crypto::hash::Sha256(base::as_byte_span(bytes)));
}

std::optional<std::string> Compress(const std::string& bytes) {
  std::string out;
  if (!compression::GzipCompress(base::span<const char>(bytes), &out)) {
    return std::nullopt;
  }
  return out;
}

// Field number in front of the server id ("32904_...").
int FieldNumberOfId(const std::string& id) {
  const size_t separator = id.find('_');
  int field = 0;
  if (separator == std::string::npos ||
      !base::StringToInt(std::string_view(id).substr(0, separator), &field)) {
    return 0;
  }
  return field;
}

int64_t WeekOf(base::Time time) {
  return (time - base::Time::UnixEpoch()).InDays() / 7;
}

std::optional<records::ReplicaIndex> ReadIndexFile(base::FilePath path) {
  std::string data;
  if (!base::ReadFileToString(path, &data)) {
    return std::nullopt;
  }
  records::ReplicaIndex index;
  if (!index.ParseFromString(data)) {
    return std::nullopt;
  }
  return index;
}

void WriteIndexFile(base::FilePath path, std::string data) {
  base::ImportantFileWriter::WriteFileAtomically(path, data);
}

struct ReconcileResult {
  LoopbackReplica::Entities exported;
  bool was_reset = false;
};

// Runs on the replica's sequence.
ReconcileResult ReconcileOnReplica(std::vector<uint8_t> keystore_key,
                                   LoopbackReplica::Entities remote,
                                   LoopbackReplica& replica) {
  ReconcileResult result;
  const std::vector<std::vector<uint8_t>> keys = replica.GetKeystoreKeys();
  if (keys.empty() || keys.back() != keystore_key) {
    // Pairing (or another vault): every device must share one keystore key,
    // so the server starts again with it and takes what the vault has. The
    // client merges its local data in at its next sync.
    replica.ResetWithKeystoreKey(std::move(keystore_key));
    replica.ImportEntities(std::move(remote));
    result.was_reset = true;
  }
  result.exported = replica.ExportEntities();
  return result;
}

}  // namespace

Replicator::Record::Record() = default;
Replicator::Record::Record(const Record&) = default;
Replicator::Record::~Record() = default;

Replicator::Replicator(ConnectorClient* client,
                       const base::FilePath& backend_file,
                       std::string device_id,
                       std::string key_prefix,
                       RefreshCallback refresh,
                       const base::Clock* clock)
    : client_(client),
      key_prefix_(std::move(key_prefix)),
      backend_file_(backend_file),
      index_file_(backend_file.DirName().Append(kIndexFileName)),
      device_id_(std::move(device_id)),
      refresh_(std::move(refresh)),
      clock_(clock ? clock : base::DefaultClock::GetInstance()) {}

Replicator::~Replicator() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (running_) {
    LoopbackBridge::Get().ClearListener(backend_file_);
  }
}

// static
syncer::DataType Replicator::TypeOf(
    const sync_pb::LoopbackServerEntity& entity) {
  return syncer::GetDataTypeFromSpecificsFieldNumber(
      FieldNumberOfId(entity.entity().id_string()));
}

// static
bool Replicator::IsReplicated(const sync_pb::LoopbackServerEntity& entity) {
  const syncer::DataType type = TypeOf(entity);
  if (!syncer::IsRealDataType(type)) {
    return false;
  }
  switch (type) {
    // Passwords live in Shard's own password store (CONNECTOR.md § 12).
    case syncer::PASSWORDS:
    case syncer::WEBAUTHN_CREDENTIAL:
    case syncer::INCOMING_PASSWORD_SHARING_INVITATION:
    case syncer::OUTGOING_PASSWORD_SHARING_INVITATION:
      return false;
    default:
      break;
  }
  // Permanent entities (bookmark root folders) are made by every server
  // alike; only Nigori, which the client changes, is shared.
  if (entity.type() == sync_pb::LoopbackServerEntity_Type_PERMANENT) {
    return type == syncer::NIGORI;
  }
  return true;
}

std::optional<std::string> Replicator::KeyFor(
    const records::Entry& entry) const {
  const sync_pb::LoopbackServerEntity& entity = entry.entity();
  const syncer::DataType type = TypeOf(entity);
  const bool deleted = entity.entity().deleted();
  if (type == syncer::HISTORY) {
    // History deletions travel as HISTORY_DELETE_DIRECTIVES.
    if (deleted || !entity.entity().specifics().has_history()) {
      return std::nullopt;
    }
    const base::Time visit = base::Time::FromDeltaSinceWindowsEpoch(
        base::Microseconds(entity.entity()
                               .specifics()
                               .history()
                               .visit_time_windows_epoch_micros()));
    const int64_t week = WeekOf(visit);
    if (week < WeekOf(clock_->Now()) - kHistoryWeeks) {
      return std::nullopt;
    }
    return kHistoryPrefix + base::NumberToString(week);
  }
  if (type == syncer::SESSIONS) {
    // A device cleans up its own stale tabs; tombstones are not needed.
    if (deleted || !entity.entity().specifics().has_session()) {
      return std::nullopt;
    }
    const uint32_t hash = base::PersistentHash(
        entity.entity().specifics().session().session_tag());
    return kSessionsPrefix + base::StringPrintf("%08x", hash);
  }
  const int field = FieldNumberOfId(entity.entity().id_string());
  auto it = shards_.find(field);
  const uint32_t shards = it == shards_.end() ? 1 : std::max(1, it->second);
  const uint32_t shard =
      base::PersistentHash(entity.entity().id_string()) % shards;
  return kTypePrefix + base::NumberToString(field) + "/" +
         base::NumberToString(shard);
}

// static
bool Replicator::Newer(const records::Entry& a, const records::Entry& b) {
  if (a.changed_ms() != b.changed_ms()) {
    return a.changed_ms() > b.changed_ms();
  }
  return a.device() > b.device();
}

// static
Replicator::Cadence Replicator::CadenceOf(const std::string& key) {
  if (key.starts_with(kHistoryPrefix)) {
    return Cadence::kHistory;
  }
  if (key.starts_with(kSessionsPrefix)) {
    return Cadence::kSessions;
  }
  return Cadence::kEntity;
}

// --- start / stop --------------------------------------------------------------

void Replicator::Start(std::vector<uint8_t> keystore_key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (running_) {
    return;
  }
  running_ = true;
  reconciled_ = false;
  keystore_key_ = std::move(keystore_key);
  stats_.error.clear();
  LoopbackBridge::Get().SetListener(backend_file_,
                                    weak_factory_.GetWeakPtr());
  ReadIndex();
}

void Replicator::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_) {
    return;
  }
  SaveIndex();
  running_ = false;
  reconciled_ = false;
  LoopbackBridge::Get().ClearListener(backend_file_);
  timers_.clear();
  index_timer_.Stop();
  weak_factory_.InvalidateWeakPtrs();
  known_.clear();
  index_.clear();
  records_.clear();
  shards_.clear();
  meta_revision_.clear();
  meta_dirty_ = meta_in_flight_ = false;
  pending_reads_ = 0;
  listing_ = false;
  keys_to_read_.clear();
}

void Replicator::ReadIndex() {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadIndexFile, index_file_),
      base::BindOnce(&Replicator::OnIndexRead, weak_factory_.GetWeakPtr()));
}

void Replicator::OnIndexRead(std::optional<records::ReplicaIndex> index) {
  if (index) {
    for (const records::IndexEntry& entry : index->entries()) {
      index_[entry.id()] = entry;
    }
  }
  ListRemote();
}

void Replicator::ListRemote() {
  if (listing_) {
    return;
  }
  listing_ = true;
  client_->List(pb::ListRequest(), base::BindOnce(&Replicator::OnListed,
                                                  weak_factory_.GetWeakPtr()));
}

void Replicator::OnListed(Status status, pb::ListReply reply) {
  listing_ = false;
  if (!status.ok()) {
    Fail("cannot list Shard records: " + status.ToString());
    // Try again later; the controller also restarts us on reconnect.
    timers_[Cadence::kEntity] = std::make_unique<base::OneShotTimer>();
    timers_[Cadence::kEntity]->Start(
        FROM_HERE, kEntityDelay,
        base::BindOnce(&Replicator::ListRemote, weak_factory_.GetWeakPtr()));
    return;
  }
  stats_.error.clear();
  keys_to_read_.clear();
  foreign_records_ = 0;
  for (const pb::Item& item : reply.items()) {
    if (item.key().starts_with(key_prefix_)) {
      keys_to_read_.push_back(item.key().substr(key_prefix_.size()));
    } else {
      ++foreign_records_;  // Other profiles and "profiles": same limit.
    }
  }
  pending_reads_ = keys_to_read_.size();
  if (pending_reads_ == 0) {
    ReconcileWithReplica();
    return;
  }
  for (const std::string& key : keys_to_read_) {
    pb::GetRequest request;
    request.set_key(key_prefix_ + key);
    client_->Get(request, base::BindOnce(&Replicator::OnRemoteRead,
                                         weak_factory_.GetWeakPtr(), key));
  }
}

void Replicator::OnRemoteRead(const std::string& key,
                              Status status,
                              pb::Item item) {
  if (status.ok()) {
    MergeRemote(key, item);
  } else {
    Fail("cannot read " + key + ": " + status.ToString());
  }
  if (--pending_reads_ == 0) {
    keys_to_read_.clear();
    ReconcileWithReplica();
  }
}

void Replicator::ReconcileWithReplica() {
  if (!running_ || pending_reads_ > 0 || !replica_available_) {
    return;  // OnReplicaAvailable / the last read comes back here.
  }
  LoopbackReplica::Entities remote;
  for (const auto& [id, entry] : known_) {
    remote.push_back(entry.entity());
  }
  const bool posted = LoopbackBridge::Get().PostToReplica(
      backend_file_,
      base::BindOnce(
          [](std::vector<uint8_t> key, LoopbackReplica::Entities remote,
             base::OnceCallback<void(ReconcileResult)> reply,
             LoopbackReplica& replica) {
            std::move(reply).Run(
                ReconcileOnReplica(std::move(key), std::move(remote), replica));
          },
          keystore_key_, std::move(remote),
          base::BindPostTaskToCurrentDefault(base::BindOnce(
              [](base::WeakPtr<Replicator> self, ReconcileResult result) {
                if (!self) {
                  return;
                }
                if (result.was_reset) {
                  // The server now holds exactly what the vault has.
                  self->reconciled_ = true;
                  self->refresh_.Run(syncer::ProtocolTypes());
                  self->FlushNow();
                  return;
                }
                self->OnExported(std::move(result.exported));
              },
              weak_factory_.GetWeakPtr()))));
  if (!posted) {
    replica_available_ = false;
  }
}

void Replicator::OnExported(LoopbackReplica::Entities entities) {
  const int64_t now = clock_->Now().InMillisecondsSinceUnixEpoch();
  std::set<std::string> local_ids;
  std::vector<records::Entry> to_import;
  for (sync_pb::LoopbackServerEntity& entity : entities) {
    if (!IsReplicated(entity)) {
      continue;
    }
    const std::string id = entity.entity().id_string();
    local_ids.insert(id);
    records::Entry local;
    auto indexed = index_.find(id);
    if (indexed != index_.end()) {
      local.set_changed_ms(indexed->second.changed_ms());
      local.set_device(indexed->second.device());
    } else {
      // Never seen by us: as old as the server says, so a vault copy that
      // is newer wins.
      local.set_changed_ms(std::min(entity.entity().mtime(), now));
      local.set_device(device_id_);
    }
    *local.mutable_entity() = std::move(entity);

    auto it = known_.find(id);
    if (it == known_.end() || Newer(local, it->second)) {
      // Ours is newer, or the vault does not have it: upload.
      std::optional<std::string> key = KeyFor(local);
      known_[id] = std::move(local);
      if (key) {
        MarkDirty(*key);
      }
    } else if (Newer(it->second, local)) {
      to_import.push_back(it->second);  // The vault's is newer.
    }
  }
  // What the vault has and the server does not.
  for (const auto& [id, entry] : known_) {
    if (!local_ids.contains(id)) {
      to_import.push_back(entry);
    }
  }
  ImportIntoReplica(std::move(to_import));
  reconciled_ = true;
  SaveIndexSoon();
  for (Cadence cadence :
       {Cadence::kEntity, Cadence::kSessions, Cadence::kHistory}) {
    ScheduleFlush(cadence);
  }
  if (meta_dirty_) {
    WriteMeta();
  }
  DropOldHistory();
  UpdateStats();
}

// --- merging -------------------------------------------------------------------

std::vector<records::Entry> Replicator::MergeRemote(const std::string& key,
                                                    const pb::Item& item) {
  std::vector<records::Entry> winners;
  if (key == kMetaKey) {
    meta_revision_ = item.revision();
    for (const pb::Version& version : item.versions()) {
      records::Meta meta;
      if (!meta.ParseFromString(version.data())) {
        continue;
      }
      for (const auto& [field, count] : meta.shards()) {
        shards_[field] =
            std::min(kMaxShardsPerType, std::max(shards_[field], count));
      }
    }
    if (item.versions_size() > 1) {
      meta_dirty_ = true;
    }
    return winners;
  }

  Record& record = records_[key];
  record.revision = item.revision();
  record.exists = item.versions_size() > 0;
  record.written_hash.clear();

  std::vector<const pb::Version*> versions;
  for (const pb::Version& version : item.versions()) {
    versions.push_back(&version);
  }
  std::sort(versions.begin(), versions.end(),
            [](const pb::Version* a, const pb::Version* b) {
              return a->id() < b->id();
            });
  for (const pb::Version* version : versions) {
    std::string raw;
    records::Shard shard;
    if (!compression::GzipUncompress(version->data(), &raw) ||
        !shard.ParseFromString(raw)) {
      // Never overwrite what we cannot read (CONNECTOR.md § 6.2).
      Fail("cannot read Shard record " + key);
      record.written_hash = "unreadable";
      return winners;
    }
    if (versions.size() == 1) {
      record.written_hash = HashOf(raw);
      record.size = version->data().size();
    }
    for (records::Entry& entry : *shard.mutable_entries()) {
      if (!IsReplicated(entry.entity())) {
        continue;
      }
      const std::string& id = entry.entity().entity().id_string();
      auto it = known_.find(id);
      if (it == known_.end() || Newer(entry, it->second)) {
        known_[id] = entry;
        winners.push_back(std::move(entry));
      }
    }
  }
  if (versions.size() > 1) {
    MarkDirty(key);  // Several devices wrote it: write the merge.
  }
  return winners;
}

void Replicator::ImportIntoReplica(std::vector<records::Entry> entries) {
  if (entries.empty()) {
    return;
  }
  LoopbackReplica::Entities entities;
  for (records::Entry& entry : entries) {
    entities.push_back(std::move(*entry.mutable_entity()));
  }
  const bool posted = LoopbackBridge::Get().PostToReplica(
      backend_file_,
      base::BindOnce(
          [](LoopbackReplica::Entities entities,
             base::OnceCallback<void(std::vector<int>)> reply,
             LoopbackReplica& replica) {
            std::move(reply).Run(replica.ImportEntities(std::move(entities)));
          },
          std::move(entities),
          base::BindPostTaskToCurrentDefault(base::BindOnce(
              &Replicator::OnImported, weak_factory_.GetWeakPtr()))));
  if (!posted) {
    // The engine is restarting; the next OnReplicaAvailable reconciles.
    replica_available_ = false;
  }
}

void Replicator::OnImported(std::vector<int> field_numbers) {
  syncer::DataTypeSet types;
  for (int field : field_numbers) {
    const syncer::DataType type =
        syncer::GetDataTypeFromSpecificsFieldNumber(field);
    if (syncer::IsRealDataType(type)) {
      types.Put(type);
    }
  }
  if (!types.empty()) {
    refresh_.Run(types);
  }
}

void Replicator::Remember(records::Entry entry) {
  const std::string id = entry.entity().entity().id_string();
  records::IndexEntry& indexed = index_[id];
  indexed.set_id(id);
  indexed.set_changed_ms(entry.changed_ms());
  indexed.set_device(entry.device());
  known_[id] = std::move(entry);
}

// --- LoopbackListener ----------------------------------------------------------

void Replicator::OnReplicaAvailable() {
  replica_available_ = true;
  if (running_) {
    ReconcileWithReplica();
  }
}

void Replicator::OnReplicaGone() {
  replica_available_ = false;
}

void Replicator::OnLocalCommit(LoopbackReplica::Entities entities) {
  if (!running_) {
    return;
  }
  const int64_t now = clock_->Now().InMillisecondsSinceUnixEpoch();
  for (sync_pb::LoopbackServerEntity& entity : entities) {
    if (!IsReplicated(entity)) {
      continue;
    }
    records::Entry entry;
    entry.set_changed_ms(now);
    entry.set_device(device_id_);
    *entry.mutable_entity() = std::move(entity);
    std::optional<std::string> key = KeyFor(entry);
    Remember(std::move(entry));
    if (key) {
      MarkDirty(*key);
    }
  }
  SaveIndexSoon();
}

// --- remote changes ------------------------------------------------------------

void Replicator::OnRecordChanged(const std::string& full_key,
                                 const std::string& revision,
                                 bool deleted) {
  if (!running_ || !reconciled_ || !full_key.starts_with(key_prefix_)) {
    return;  // The startup listing reads it, or another profile's record.
  }
  const std::string key = full_key.substr(key_prefix_.size());
  if (key == kMetaKey) {
    if (revision == meta_revision_) {
      return;
    }
  } else {
    auto it = records_.find(key);
    if (it != records_.end() && it->second.revision == revision) {
      return;  // Our own write.
    }
  }
  if (deleted && key != kMetaKey) {
    Record& record = records_[key];
    record.revision = revision;
    record.exists = false;
    record.written_hash.clear();
    MarkDirty(key);  // Rewritten only if we still have entries for it.
    return;
  }
  pb::GetRequest request;
  request.set_key(key_prefix_ + key);
  client_->Get(request,
               base::BindOnce(
                   [](base::WeakPtr<Replicator> self, std::string key,
                      Status status, pb::Item item) {
                     if (!self || !status.ok()) {
                       return;
                     }
                     self->ImportIntoReplica(self->MergeRemote(key, item));
                     if (key != kMetaKey) {
                       // Rewritten only if ours differs from the vault's.
                       self->MarkDirty(key);
                     }
                     self->UpdateStats();
                   },
                   weak_factory_.GetWeakPtr(), key));
}

void Replicator::OnWritable() {
  if (!running_) {
    return;
  }
  if (!reconciled_) {
    if (!listing_ && pending_reads_ == 0) {
      ListRemote();  // The startup listing failed earlier.
    }
    return;
  }
  FlushNow();
}

void Replicator::FlushNow() {
  for (Cadence cadence :
       {Cadence::kEntity, Cadence::kSessions, Cadence::kHistory}) {
    FlushCadence(cadence);
  }
}

// --- writing -------------------------------------------------------------------

void Replicator::MarkDirty(const std::string& key) {
  Record& record = records_[key];
  if (record.in_flight) {
    record.dirty_again = true;
    return;
  }
  record.dirty = true;
  ScheduleFlush(CadenceOf(key));
}

void Replicator::ScheduleFlush(Cadence cadence) {
  if (!reconciled_) {
    return;
  }
  std::unique_ptr<base::OneShotTimer>& timer = timers_[cadence];
  if (!timer) {
    timer = std::make_unique<base::OneShotTimer>();
  }
  if (timer->IsRunning()) {
    return;
  }
  base::TimeDelta delay = kEntityDelay;
  if (cadence == Cadence::kSessions) {
    delay = kSessionsDelay;
  } else if (cadence == Cadence::kHistory) {
    delay = kHistoryDelay;
  }
  timer->Start(FROM_HERE, delay,
               base::BindOnce(&Replicator::FlushCadence,
                              weak_factory_.GetWeakPtr(), cadence));
}

void Replicator::FlushCadence(Cadence cadence) {
  if (!running_ || !reconciled_) {
    return;
  }
  if (cadence == Cadence::kEntity && meta_dirty_) {
    WriteMeta();
  }
  if (cadence == Cadence::kHistory) {
    DropOldHistory();
  }
  std::map<std::string, std::vector<const records::Entry*>> groups;
  for (const auto& [key, record] : records_) {
    if (record.dirty && !record.in_flight && CadenceOf(key) == cadence) {
      groups[key];
    }
  }
  if (groups.empty()) {
    return;
  }
  // One pass over everything we know, however many records are dirty.
  for (const auto& [id, entry] : known_) {
    std::optional<std::string> key = KeyFor(entry);
    if (!key) {
      continue;
    }
    auto group = groups.find(*key);
    if (group != groups.end()) {
      group->second.push_back(&entry);
    }
  }
  for (const auto& [key, entries] : groups) {
    if (!Write(key, entries) || !running_) {
      return;  // A type was split: the grouping is stale, a new flush runs.
    }
  }
}

std::optional<std::string> Replicator::BuildValue(
    const std::string& key,
    const std::vector<const records::Entry*>& entries) {
  const int64_t tombstone_cutoff =
      (clock_->Now() - kTombstoneLifetime).InMillisecondsSinceUnixEpoch();
  records::Shard shard;
  shard.set_format(kFormat);
  for (const records::Entry* entry : entries) {
    if (entry->entity().entity().deleted() &&
        entry->changed_ms() < tombstone_cutoff) {
      continue;
    }
    *shard.add_entries() = *entry;
  }
  if (shard.entries_size() == 0) {
    return std::nullopt;
  }
  if (!key.starts_with(kTypePrefix)) {
    // History weeks and device sessions cannot be split: keep the newest
    // entries that fit.
    std::string raw = shard.SerializeAsString();
    std::optional<std::string> packed = Compress(raw);
    while (packed && packed->size() > kMaxRecordBytes &&
           shard.entries_size() > 1) {
      auto* entries = shard.mutable_entries();
      std::sort(entries->begin(), entries->end(),
                [](const records::Entry& a, const records::Entry& b) {
                  return a.changed_ms() > b.changed_ms();
                });
      entries->DeleteSubrange(entries->size() / 2 + entries->size() / 4,
                              entries->size() - entries->size() / 2 -
                                  entries->size() / 4);
      raw = shard.SerializeAsString();
      packed = Compress(raw);
    }
    // Keep a stable order for the content hash.
    auto* entries = shard.mutable_entries();
    std::sort(entries->begin(), entries->end(),
              [](const records::Entry& a, const records::Entry& b) {
                return a.entity().entity().id_string() <
                       b.entity().entity().id_string();
              });
  }
  return shard.SerializeAsString();
}

bool Replicator::Write(const std::string& key,
                       const std::vector<const records::Entry*>& entries) {
  Record& record = records_[key];
  if (record.written_hash == "unreadable") {
    record.dirty = false;  // Left alone; the error is shown.
    return true;
  }
  std::optional<std::string> raw = BuildValue(key, entries);
  if (!raw) {
    record.dirty = false;
    if (!record.exists) {
      return true;
    }
    record.in_flight = true;
    pb::DeleteRequest request;
    request.set_key(key_prefix_ + key);
    request.set_base(record.revision);
    client_->Delete(request, base::BindOnce(&Replicator::OnWritten,
                                            weak_factory_.GetWeakPtr(), key,
                                            std::string(), 0));
    return true;
  }
  const std::string hash = HashOf(*raw);
  if (record.exists && hash == record.written_hash) {
    record.dirty = false;  // The vault already has exactly this.
    return true;
  }
  std::optional<std::string> packed = Compress(*raw);
  if (!packed) {
    Fail("cannot compress " + key);
    return true;
  }
  if (packed->size() > kMaxShardBytes && key.starts_with(kTypePrefix)) {
    const std::string rest = key.substr(sizeof(kTypePrefix) - 1);
    int field = 0;
    if (base::StringToInt(rest.substr(0, rest.find('/')), &field) &&
        shards_[field] < kMaxShardsPerType) {
      SplitType(field);
      return false;
    }
  }
  if (packed->size() > kMaxRecordBytes) {
    Fail(key + " is larger than a Shard record");
    record.dirty = false;
    return true;
  }
  if (!record.exists) {
    size_t existing = 0;
    for (const auto& [other_key, other] : records_) {
      existing += other.exists ? 1 : 0;
    }
    if (existing + foreign_records_ + 1 >= kMaxRecords) {
      Fail("Shard's record limit for Enclave is reached");
      return true;
    }
  }
  record.dirty = false;
  record.in_flight = true;
  const size_t size = packed->size();
  pb::PutRequest request;
  request.set_key(key_prefix_ + key);
  request.set_data(std::move(*packed));
  request.set_base(record.revision);
  client_->Put(request, base::BindOnce(&Replicator::OnWritten,
                                       weak_factory_.GetWeakPtr(), key, hash,
                                       size));
  return true;
}

void Replicator::OnWritten(const std::string& key,
                           std::string hash,
                           size_t size,
                           Status status,
                           pb::Item item) {
  Record& record = records_[key];
  record.in_flight = false;
  if (status.ok()) {
    record.revision = item.revision();
    record.exists = item.versions_size() > 0;
    record.written_hash = std::move(hash);
    record.size = record.exists ? size : 0;
    stats_.last_write = clock_->Now();
    if (stats_.error.starts_with("cannot write")) {
      stats_.error.clear();
    }
    if (record.dirty_again) {
      record.dirty_again = false;
      MarkDirty(key);
    }
    UpdateStats();
    return;
  }
  record.dirty_again = false;
  switch (status.code) {
    case Code::kAborted: {
      // Someone wrote meanwhile: read, merge, write again (§ 6.1).
      pb::GetRequest request;
      request.set_key(key_prefix_ + key);
      client_->Get(request,
                   base::BindOnce(&Replicator::OnRereadAfterConflict,
                                  weak_factory_.GetWeakPtr(), key));
      return;
    }
    case Code::kUnavailable:
    case Code::kFailedPrecondition:
    case Code::kDeadlineExceeded:
      record.dirty = true;  // Pending until OnWritable.
      return;
    default:
      record.dirty = true;
      Fail("cannot write " + key + ": " + status.ToString());
      return;
  }
}

void Replicator::OnRereadAfterConflict(const std::string& key,
                                       Status status,
                                       pb::Item item) {
  if (!status.ok()) {
    records_[key].dirty = true;
    return;
  }
  ImportIntoReplica(MergeRemote(key, item));
  MarkDirty(key);
  FlushCadence(CadenceOf(key));
}

void Replicator::SplitType(int field_number) {
  const int32_t old_count = std::max(1, shards_[field_number]);
  shards_[field_number] = std::min(kMaxShardsPerType, old_count * 2);
  meta_dirty_ = true;
  WriteMeta();
  const std::string prefix =
      kTypePrefix + base::NumberToString(field_number) + "/";
  for (int32_t i = 0; i < shards_[field_number]; ++i) {
    Record& record = records_[prefix + base::NumberToString(i)];
    record.dirty = true;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&Replicator::FlushCadence,
                                weak_factory_.GetWeakPtr(), Cadence::kEntity));
}

void Replicator::WriteMeta() {
  if (meta_in_flight_) {
    return;
  }
  meta_dirty_ = false;
  meta_in_flight_ = true;
  records::Meta meta;
  meta.set_format(kFormat);
  for (const auto& [field, count] : shards_) {
    if (count > 1) {
      (*meta.mutable_shards())[field] = count;
    }
  }
  pb::PutRequest request;
  request.set_key(key_prefix_ + kMetaKey);
  request.set_data(meta.SerializeAsString());
  request.set_base(meta_revision_);
  client_->Put(
      request,
      base::BindOnce(
          [](base::WeakPtr<Replicator> self, Status status, pb::Item item) {
            if (!self) {
              return;
            }
            self->meta_in_flight_ = false;
            if (status.ok()) {
              self->meta_revision_ = item.revision();
              return;
            }
            self->meta_dirty_ = true;
            if (status.code != Code::kAborted) {
              return;  // Retried with the next entity flush.
            }
            pb::GetRequest get;
            get.set_key(self->key_prefix_ + kMetaKey);
            self->client_->Get(
                get, base::BindOnce(
                         [](base::WeakPtr<Replicator> self, Status status,
                            pb::Item item) {
                           if (self && status.ok()) {
                             self->MergeRemote(kMetaKey, item);
                             self->meta_dirty_ = true;
                             self->WriteMeta();
                           }
                         },
                         self));
          },
          weak_factory_.GetWeakPtr()));
}

void Replicator::DropOldHistory() {
  const int64_t oldest = WeekOf(clock_->Now()) - kHistoryWeeks;
  for (auto& [key, record] : records_) {
    if (!key.starts_with(kHistoryPrefix) || !record.exists ||
        record.in_flight) {
      continue;
    }
    int64_t week = 0;
    if (base::StringToInt64(key.substr(sizeof(kHistoryPrefix) - 1), &week) &&
        week < oldest) {
      // BuildValue finds nothing for it any more: Write deletes it.
      record.dirty = true;
    }
  }
  std::erase_if(known_, [&](const auto& id_and_entry) {
    const records::Entry& entry = id_and_entry.second;
    return TypeOf(entry.entity()) == syncer::HISTORY && !KeyFor(entry);
  });
}

void Replicator::RemoveAllRecords(base::OnceCallback<void(Status)> done) {
  pb::ListRequest list;
  list.set_prefix(key_prefix_);
  client_->List(
      list,
      base::BindOnce(
          [](base::WeakPtr<Replicator> self,
             base::OnceCallback<void(Status)> done, Status status,
             pb::ListReply reply) {
            if (!self || !status.ok() || reply.items_size() == 0) {
              std::move(done).Run(status);
              return;
            }
            auto remaining = std::make_shared<int>(reply.items_size());
            auto first_error = std::make_shared<Status>();
            auto shared_done = std::make_shared<base::OnceCallback<void(Status)>>(
                std::move(done));
            for (const pb::Item& item : reply.items()) {
              pb::DeleteRequest request;
              request.set_key(item.key());
              request.set_base(item.revision());
              self->client_->Delete(
                  request, base::BindOnce(
                               [](std::shared_ptr<int> remaining,
                                  std::shared_ptr<Status> first_error,
                                  std::shared_ptr<
                                      base::OnceCallback<void(Status)>> done,
                                  Status status, pb::Item) {
                                 if (!status.ok() && first_error->ok()) {
                                   *first_error = status;
                                 }
                                 if (--*remaining == 0) {
                                   std::move(*done).Run(*first_error);
                                 }
                               },
                               remaining, first_error, shared_done));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

// --- bookkeeping ---------------------------------------------------------------

void Replicator::UpdateStats() {
  stats_.other_records = foreign_records_;
  stats_.records = meta_revision_.empty() ? 0 : 1;
  stats_.bytes = 0;
  for (const auto& [key, record] : records_) {
    if (record.exists) {
      ++stats_.records;
      stats_.bytes += record.size;
    }
  }
}

void Replicator::SaveIndexSoon() {
  if (!index_timer_.IsRunning()) {
    index_timer_.Start(FROM_HERE, base::Seconds(10),
                       base::BindOnce(&Replicator::SaveIndex,
                                      weak_factory_.GetWeakPtr()));
  }
}

void Replicator::SaveIndex() {
  records::ReplicaIndex index;
  for (const auto& [id, entry] : known_) {
    records::IndexEntry* indexed = index.add_entries();
    indexed->set_id(id);
    indexed->set_changed_ms(entry.changed_ms());
    indexed->set_device(entry.device());
  }
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(&WriteIndexFile, index_file_, index.SerializeAsString()));
}

void Replicator::Fail(std::string error) {
  LOG(WARNING) << "Shard replication: " << error;
  stats_.error = std::move(error);
}

}  // namespace enclave::shard
