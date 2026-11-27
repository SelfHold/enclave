// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/bezel/cloudsync/bezel_password_mirror.h"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/bezel/cloudsync/bezel_password_conversion.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/bezel_cloudsync/connector_client.h"
#include "components/bezel_cloudsync/status.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/password_store/password_form_converters.h"
#include "components/password_manager/core/browser/password_store/password_store_backend_error.h"
#include "components/password_manager/core/browser/password_store/password_store_change.h"
#include "components/password_manager/core/browser/password_store/password_store_consumer.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/password_manager/core/browser/password_store/stored_credential.h"

namespace bezel {

namespace {

namespace ps = password_sync;
namespace pb = cloudsync::pb;
using cloudsync::Code;
using cloudsync::Status;
using password_manager::CloneStoredCredential;
using password_manager::PasswordStoreChange;
using password_manager::PasswordStoreChangeList;
using password_manager::PasswordStoreInterface;
using password_manager::StoredCredential;

constexpr base::FilePath::CharType kDirName[] = FILE_PATH_LITERAL("Bezel Sync");
constexpr base::FilePath::CharType kFileName[] =
    FILE_PATH_LITERAL("cloudsync_logins.json");
constexpr int kFileVersion = 1;
constexpr size_t kMaxFileBytes = 32 * 1024 * 1024;

constexpr base::TimeDelta kPollInterval = base::Minutes(5);
constexpr base::TimeDelta kLocalDebounce = base::Seconds(2);
constexpr base::TimeDelta kSaveDelay = base::Seconds(2);
constexpr size_t kIdsPerCall = 500;  // ListLogins pages, GetLogins (§ 12.2).
constexpr int kMaxAbortedRetries = 3;

std::optional<std::string> ReadMappingFile(base::FilePath path) {
  std::string text;
  if (!base::ReadFileToStringWithMaxSize(path, &text, kMaxFileBytes)) {
    return std::nullopt;
  }
  return text;
}

void WriteMappingFile(base::FilePath path, std::string text) {
  if (!base::CreateDirectory(path.DirName())) {
    return;
  }
  base::ImportantFileWriter::WriteFileAtomically(path, text);
#if BUILDFLAG(IS_POSIX)
  base::SetPosixFilePermissions(path, base::FILE_PERMISSION_READ_BY_USER |
                                          base::FILE_PERMISSION_WRITE_BY_USER);
#endif
}

std::string MsToString(int64_t ms) {
  return base::NumberToString(ms);
}

int64_t MsFromValue(const std::string* text) {
  int64_t ms = 0;
  if (text) {
    base::StringToInt64(*text, &ms);
  }
  return ms;
}

// Calls that cannot work until the user acts in CloudSync.
bool IsFatal(const Status& status) {
  return !status.transport && (status.code == Code::kUnauthenticated ||
                               status.code == Code::kPermissionDenied);
}

// Writes stay pending and are retried (§ 9, § 12.3).
bool IsRetryable(const Status& status) {
  if (status.transport) {
    return true;
  }
  switch (status.code) {
    case Code::kUnavailable:
    case Code::kFailedPrecondition:
    case Code::kDeadlineExceeded:
    case Code::kResourceExhausted:
    case Code::kCancelled:
    case Code::kUnknown:
    case Code::kInternal:
      return true;
    default:
      return false;
  }
}

}  // namespace

class BezelPasswordMirror::Impl
    : public cloudsync::CloudSyncController::Observer,
      public PasswordStoreInterface::Observer,
      public password_manager::PasswordStoreConsumer {
 public:
  Impl(Profile* profile, cloudsync::CloudSyncController* controller);
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl() override;

  void Shutdown();

  // cloudsync::CloudSyncController::Observer:
  void OnCloudSyncStateChanged() override;
  void OnCloudSyncLoginsChanged() override;

  // PasswordStoreInterface::Observer:
  void OnLoginsChanged(PasswordStoreInterface* store,
                       const PasswordStoreChangeList& changes) override;
  void OnLoginsRetained(
      PasswordStoreInterface* store,
      const std::vector<StoredCredential>& retained_credentials) override;

  // PasswordStoreConsumer:
  void OnGetPasswordStoreResultsOrErrorFrom(
      PasswordStoreInterface* store,
      base::expected<std::vector<StoredCredential>,
                     password_manager::PasswordStoreBackendError>
          results_or_error) override;

 private:
  // What this device last synced for one CloudSync login. Never a password.
  struct Entry {
    std::string revision;
    ps::LoginKey key;
    std::string hash;          // ps::ContentHash of the local copy.
    int64_t password_ms = 0;   // Its date_password_modified.
    int64_t used_ms = 0;       // Its date_last_used, as last sent.
    int times_used = 0;        // Its times_used_in_html_form.
    int64_t change_ms = 0;     // When a local change not yet written was seen.
    bool pending = false;      // A local change waits for CloudSync.
    // Another login holds the same unique key in the login DB (a conflict
    // copy "<id>~<op id>" of the same account): this one is not stored
    // locally; it is looked at again when its key becomes free.
    bool shadow = false;
    // CloudSync holds it without a profile tag (primary profile only).
    bool untagged = false;
  };

  struct Op {
    enum class Kind { kPut, kDelete, kTouch };
    Kind kind = Kind::kPut;
    std::string id;    // "" = a new login.
    std::string base;  // Revision the change was made from.
    StoredCredential cred;
    std::optional<pb::Login> remote;  // For other apps' extra keys.
    int64_t used_ms = 0;
    int attempts = 0;
    // Write it back without a profile tag: an untagged login the user did
    // not change here (first merge).
    bool keep_untagged = false;
  };

  // One pass: local snapshot + remote delta (or full list) -> writes.
  struct Cycle {
    bool full = false;
    std::vector<StoredCredential> locals;
    std::map<ps::LoginKey, size_t> local_index;
    std::vector<std::string> local_hash;  // Lazily filled.
    std::set<size_t> claimed;
    // Local copies re-keyed (username edited) before the snapshot.
    std::map<std::string, ps::LoginKey> renames;
    std::map<std::string, std::string> remote_revs;     // Full mode.
    std::map<std::string, pb::LoginChange> changes;     // Delta mode.
    std::string new_cursor;
    std::vector<std::string> to_fetch;
    size_t fetch_pos = 0;
    std::map<std::string, pb::Login> fetched;
    // Fetched logins of other profiles: ignored.
    std::set<std::string> not_ours;
    std::deque<Op> ops;
  };

  void UpdateRunning();
  void Start();
  void Stop();
  void Begin();
  void Load();
  void OnLoaded(std::optional<std::string> text);
  void SaveSoon();
  void SaveNow();

  void RequestCycle(bool full);
  void RequestCycleSoon();
  void StartCycle();
  void ListAll(std::string page_token);
  void OnListPage(Status status, pb::ListLoginsReply reply);
  void OnListChanges(Status status, pb::ListChangesReply reply);
  void PlanFetch();
  void FetchNext();
  void OnFetched(Status status, pb::ListLoginsReply reply);
  void Reconcile();
  void EndCycle(bool remote_applied);
  // A read failed: stop on auth errors, else retry later.
  void OnReadFailed(const Status& status);

  // Reconcile helpers.
  const StoredCredential* LocalFor(const std::string& id,
                                   Entry& entry,
                                   size_t* index);
  const std::string& LocalHash(size_t index);
  bool LocalChanged(const Entry& entry, size_t index);
  void HandleNewRemote(const pb::Login& remote);
  void ApplyRemote(const std::string& id,
                   const pb::Login& remote,
                   const StoredCredential* local);
  void SetEntry(const std::string& id,
                const std::string& revision,
                const StoredCredential& cred,
                bool untagged);
  void SetShadow(const std::string& id,
                 const std::string& revision,
                 const ps::LoginKey& key);
  void DropEntry(const std::string& id);
  void QueuePut(const std::string& id,
                const std::string& base,
                const StoredCredential& cred,
                const pb::Login* remote,
                bool keep_untagged = false);
  bool Owns(const pb::Login& login) const;
  void QueueDelete(const std::string& id, const std::string& base);
  void QueueTouch(const std::string& id, const StoredCredential& cred);

  // Local writes, recorded so that their echoes are not written back.
  void AddLocal(const StoredCredential& cred);
  void UpdateLocal(const StoredCredential& cred);
  void RemoveLocal(const StoredCredential& cred);
  bool ConsumeExpected(const ps::LoginKey& key, const std::string& hash);

  // Remote writes, one at a time.
  void ProcessNextOp();
  void OnPut(Status status, pb::Login login);
  void OnDelete(Status status, pb::DeleteLoginReply reply);
  void OnTouch(Status status, pb::TouchLoginReply reply);
  void OnRefetched(Status status, pb::ListLoginsReply reply);
  void OnWriteFailed(const Status& status);
  void SendTouch(const std::string& id, int64_t used_ms);
  void StopForError(const Status& status);

  cloudsync::ConnectorClient* client() { return controller_->client(); }

  const raw_ptr<cloudsync::CloudSyncController> controller_;
  scoped_refptr<PasswordStoreInterface> store_;
  const base::FilePath file_;
  scoped_refptr<base::SequencedTaskRunner> file_runner_;

  bool shut_down_ = false;
  bool loaded_ = false;
  bool loading_ = false;
  bool running_ = false;
  bool observing_store_ = false;
  bool stopped_for_error_ = false;
  bool busy_ = false;
  bool cycle_wanted_ = false;
  bool full_wanted_ = false;
  bool retry_on_ready_ = false;
  cloudsync::CloudSyncController::Phase last_phase_ =
      cloudsync::CloudSyncController::Phase::kStarting;

  // Mapping, saved in `file_`. Its identity is (vault, profile slot); the
  // primary flag decides who owns untagged logins.
  std::string vault_id_;
  std::string slot_;
  bool primary_ = false;
  std::string cursor_;
  // Logins of other profiles, id -> revision: unchanged ones are not
  // fetched again to learn their owner.
  std::map<std::string, std::string> foreign_;
  std::map<std::string, Entry> entries_;
  std::map<ps::LoginKey, std::string> by_key_;  // Non-shadow entries only.

  // Echo suppression: local writes made from CloudSync data, by key, with
  // the content hash written ("" = removal).
  std::multimap<ps::LoginKey, std::string> expected_;
  // Writes CloudSync refused as invalid: not retried until restart.
  std::set<ps::LoginKey> rejected_;
  // Entry id -> new key of its local copy: UpdateLoginWithPrimaryKey shows
  // up as REMOVE old + ADD new in one change list.
  std::map<std::string, ps::LoginKey> renames_;

  std::unique_ptr<Cycle> cycle_;

  base::RepeatingTimer poll_timer_;
  base::OneShotTimer debounce_timer_;
  base::OneShotTimer save_timer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<password_manager::PasswordStoreConsumer>
      consumer_weak_factory_{this};
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

BezelPasswordMirror::Impl::Impl(Profile* profile,
                                cloudsync::CloudSyncController* controller)
    : controller_(controller),
      store_(ProfilePasswordStoreFactory::GetForProfile(
          profile,
          ServiceAccessType::EXPLICIT_ACCESS)),
      file_(profile->GetPath().Append(kDirName).Append(kFileName)),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  controller_->AddObserver(this);
  last_phase_ = controller_->state().phase;
  UpdateRunning();
}

BezelPasswordMirror::Impl::~Impl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Shutdown();
}

void BezelPasswordMirror::Impl::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shut_down_) {
    return;
  }
  Stop();
  if (save_timer_.IsRunning()) {
    SaveNow();
  }
  shut_down_ = true;
  controller_->RemoveObserver(this);
  store_.reset();
}

// --- Running state ---------------------------------------------------------

void BezelPasswordMirror::Impl::UpdateRunning() {
  const bool usable = !shut_down_ && store_ &&
                      controller_->CanUsePasswords() &&
                      !controller_->profile_slot().empty();
  if (!usable) {
    stopped_for_error_ = false;
    if (running_) {
      Stop();
    }
    return;
  }
  if (!running_ && !stopped_for_error_) {
    Start();
  }
}

void BezelPasswordMirror::Impl::Start() {
  running_ = true;
  if (!loaded_) {
    Load();
    return;
  }
  Begin();
}

void BezelPasswordMirror::Impl::Begin() {
  if (!running_) {
    return;
  }
  const std::string& vault = controller_->state().vault_id;
  const std::string& slot = controller_->profile_slot();
  const bool primary = controller_->is_primary_slot();
  if (vault != vault_id_ || slot != slot_) {
    // Another vault or another slot: what this device knew is worthless.
    entries_.clear();
    by_key_.clear();
    foreign_.clear();
    cursor_.clear();
    vault_id_ = vault;
    slot_ = slot;
    primary_ = primary;
    SaveNow();
  } else if (primary != primary_) {
    // Untagged logins changed hands: learn every owner again.
    primary_ = primary;
    foreign_.clear();
    full_wanted_ = true;
    SaveNow();
  }
  if (!observing_store_) {
    store_->AddObserver(this);
    observing_store_ = true;
  }
  poll_timer_.Start(FROM_HERE, kPollInterval,
                    base::BindRepeating(&Impl::RequestCycle,
                                        base::Unretained(this), false));
  RequestCycle(false);
}

void BezelPasswordMirror::Impl::Stop() {
  if (!running_ && !loading_) {
    return;
  }
  running_ = false;
  loading_ = false;
  if (observing_store_ && store_) {
    store_->RemoveObserver(this);
  }
  observing_store_ = false;
  poll_timer_.Stop();
  debounce_timer_.Stop();
  weak_factory_.InvalidateWeakPtrs();
  consumer_weak_factory_.InvalidateWeakPtrs();
  cycle_.reset();
  busy_ = false;
  cycle_wanted_ = false;
  full_wanted_ = false;
  expected_.clear();
  if (loaded_) {
    SaveNow();
  }
}

void BezelPasswordMirror::Impl::StopForError(const Status& status) {
  LOG(WARNING) << "Bezel: CloudSync refused the password store: "
               << status.ToString();
  stopped_for_error_ = true;
  Stop();
}

void BezelPasswordMirror::Impl::OnCloudSyncStateChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  using Phase = cloudsync::CloudSyncController::Phase;
  const Phase phase = controller_->state().phase;
  const Phase previous = last_phase_;
  last_phase_ = phase;

  if (running_ && loaded_ &&
      (controller_->state().vault_id != vault_id_ ||
       controller_->profile_slot() != slot_ ||
       controller_->is_primary_slot() != primary_)) {
    Stop();  // Begin() drops the mapping, or relearns the owners.
  }
  const bool was_running = running_;
  UpdateRunning();
  if (!running_ || !was_running) {
    return;
  }
  // Back to READY (reconnect, unlock, storage back): catch up and retry
  // pending writes.
  if (phase == Phase::kReady &&
      (previous != Phase::kReady || retry_on_ready_)) {
    retry_on_ready_ = false;
    RequestCycle(false);
  }
}

void BezelPasswordMirror::Impl::OnCloudSyncLoginsChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RequestCycle(false);
}

// --- Mapping file ----------------------------------------------------------

void BezelPasswordMirror::Impl::Load() {
  if (loading_) {
    return;
  }
  loading_ = true;
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadMappingFile, file_),
      base::BindOnce(&Impl::OnLoaded, weak_factory_.GetWeakPtr()));
}

void BezelPasswordMirror::Impl::OnLoaded(std::optional<std::string> text) {
  loading_ = false;
  loaded_ = true;
  entries_.clear();
  by_key_.clear();
  foreign_.clear();
  std::optional<base::DictValue> dict;
  if (text) {
    dict = base::JSONReader::ReadDict(*text, base::JSON_PARSE_RFC);
  }
  if (dict && dict->FindInt("version") == kFileVersion) {
    if (const std::string* vault = dict->FindString("vault_id")) {
      vault_id_ = *vault;
    }
    if (const std::string* slot = dict->FindString("slot")) {
      slot_ = *slot;
    }
    primary_ = dict->FindBool("primary").value_or(false);
    if (const std::string* cursor = dict->FindString("cursor")) {
      cursor_ = *cursor;
    }
    if (const base::DictValue* foreign = dict->FindDict("foreign")) {
      for (const auto [id, value] : *foreign) {
        if (const std::string* rev = value.GetIfString()) {
          foreign_[id] = *rev;
        }
      }
    }
    if (const base::DictValue* logins = dict->FindDict("logins")) {
      for (const auto [id, value] : *logins) {
        const base::DictValue* item = value.GetIfDict();
        const base::ListValue* key = item ? item->FindList("key") : nullptr;
        if (!item || !key || key->size() != 5) {
          continue;
        }
        Entry entry;
        bool key_ok = true;
        for (size_t i = 0; i < 5; ++i) {
          const std::string* part = (*key)[i].GetIfString();
          if (!part) {
            key_ok = false;
            break;
          }
          entry.key[i] = *part;
        }
        if (!key_ok) {
          continue;
        }
        if (const std::string* rev = item->FindString("rev")) {
          entry.revision = *rev;
        }
        if (const std::string* hash = item->FindString("hash")) {
          entry.hash = *hash;
        }
        entry.password_ms = MsFromValue(item->FindString("pw_ms"));
        entry.used_ms = MsFromValue(item->FindString("used_ms"));
        entry.times_used = item->FindInt("times").value_or(0);
        entry.change_ms = MsFromValue(item->FindString("change_ms"));
        entry.pending = item->FindBool("pending").value_or(false);
        entry.shadow = item->FindBool("shadow").value_or(false);
        entry.untagged = item->FindBool("untagged").value_or(false);
        if (!entry.shadow) {
          if (by_key_.contains(entry.key)) {
            entry.shadow = true;
          } else {
            by_key_[entry.key] = id;
          }
        }
        entries_[id] = std::move(entry);
      }
    }
  } else {
    vault_id_.clear();
    slot_.clear();
    cursor_.clear();
    foreign_.clear();
  }
  Begin();
}

void BezelPasswordMirror::Impl::SaveSoon() {
  if (!save_timer_.IsRunning()) {
    save_timer_.Start(FROM_HERE, kSaveDelay,
                      base::BindOnce(&Impl::SaveNow, base::Unretained(this)));
  }
}

void BezelPasswordMirror::Impl::SaveNow() {
  save_timer_.Stop();
  if (!loaded_) {
    return;
  }
  base::DictValue logins;
  for (const auto& [id, entry] : entries_) {
    base::ListValue key;
    for (const std::string& part : entry.key) {
      key.Append(part);
    }
    base::DictValue item;
    item.Set("rev", entry.revision);
    item.Set("key", std::move(key));
    item.Set("hash", entry.hash);
    item.Set("pw_ms", MsToString(entry.password_ms));
    item.Set("used_ms", MsToString(entry.used_ms));
    item.Set("times", entry.times_used);
    if (entry.change_ms) {
      item.Set("change_ms", MsToString(entry.change_ms));
    }
    if (entry.pending) {
      item.Set("pending", true);
    }
    if (entry.shadow) {
      item.Set("shadow", true);
    }
    if (entry.untagged) {
      item.Set("untagged", true);
    }
    logins.Set(id, std::move(item));
  }
  base::DictValue foreign;
  for (const auto& [id, revision] : foreign_) {
    foreign.Set(id, revision);
  }
  base::DictValue dict;
  dict.Set("version", kFileVersion);
  dict.Set("vault_id", vault_id_);
  dict.Set("slot", slot_);
  dict.Set("primary", primary_);
  dict.Set("cursor", cursor_);
  dict.Set("logins", std::move(logins));
  dict.Set("foreign", std::move(foreign));
  std::optional<std::string> text = base::WriteJson(dict);
  if (!text) {
    return;
  }
  file_runner_->PostTask(FROM_HERE, base::BindOnce(&WriteMappingFile, file_,
                                                   std::move(*text)));
}

// --- Local changes ---------------------------------------------------------

void BezelPasswordMirror::Impl::OnLoginsChanged(
    PasswordStoreInterface* store,
    const PasswordStoreChangeList& changes) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_ || !loaded_) {
    return;
  }
  const int64_t now_ms = ps::ToMs(base::Time::Now());
  bool need_cycle = false;
  bool dirty = false;
  std::vector<std::string> removed_ids;
  std::vector<ps::LoginKey> added_keys;

  for (const PasswordStoreChange& change : changes) {
    const StoredCredential& cred = change.credential();
    const ps::LoginKey key = ps::KeyOf(cred);
    const bool removal = change.type() == PasswordStoreChange::REMOVE;
    const std::string hash = removal ? std::string() : ps::ContentHash(cred);
    if (ConsumeExpected(key, hash)) {
      continue;  // Our own write from CloudSync data.
    }
    auto owner = by_key_.find(key);
    if (owner == by_key_.end()) {
      if (!removal) {
        added_keys.push_back(key);
        need_cycle = true;  // A new local login.
      }
      continue;
    }
    Entry& entry = entries_[owner->second];
    if (removal) {
      entry.pending = true;
      entry.change_ms = now_ms;
      removed_ids.push_back(owner->second);
      need_cycle = dirty = true;
      continue;
    }
    if (hash != entry.hash ||
        ps::ToMs(cred.date_password_modified) != entry.password_ms ||
        change.password_changed()) {
      entry.pending = true;
      entry.change_ms = now_ms;
      need_cycle = dirty = true;
      continue;
    }
    // Content unchanged: a fill that only moved the use counters (§ 12.4).
    if (ps::UseCountersAdvanced(cred, entry.used_ms, entry.times_used)) {
      entry.used_ms = ps::ToMs(cred.date_last_used);
      entry.times_used = cred.times_used_in_html_form;
      SendTouch(owner->second, entry.used_ms);
      dirty = true;
    }
  }
  // UpdateLoginWithPrimaryKey (username edited) is REMOVE old + ADD new.
  if (removed_ids.size() == 1 && added_keys.size() == 1) {
    renames_[removed_ids[0]] = added_keys[0];
  }
  if (dirty) {
    SaveSoon();
  }
  if (need_cycle) {
    RequestCycleSoon();
  }
}

void BezelPasswordMirror::Impl::OnLoginsRetained(
    PasswordStoreInterface* store,
    const std::vector<StoredCredential>& retained_credentials) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The store was reloaded as a whole: compare everything.
  RequestCycleSoon();
}

bool BezelPasswordMirror::Impl::ConsumeExpected(const ps::LoginKey& key,
                                                const std::string& hash) {
  auto [begin, end] = expected_.equal_range(key);
  for (auto it = begin; it != end; ++it) {
    if (it->second == hash) {
      expected_.erase(it);
      return true;
    }
  }
  return false;
}

void BezelPasswordMirror::Impl::SendTouch(const std::string& id,
                                          int64_t used_ms) {
  cloudsync::ConnectorClient* connector = client();
  if (!connector) {
    return;
  }
  pb::TouchLoginRequest request;
  request.set_id(id);
  request.set_used_ms(used_ms);
  connector->TouchLogin(
      request, base::BindOnce(
                   [](base::WeakPtr<Impl> self, Status status,
                      pb::TouchLoginReply) {
                     if (self && IsFatal(status)) {
                       self->StopForError(status);
                     }
                   },
                   weak_factory_.GetWeakPtr()));
}

// --- Sync cycle ------------------------------------------------------------

void BezelPasswordMirror::Impl::RequestCycle(bool full) {
  if (!running_ || !loaded_) {
    return;
  }
  cycle_wanted_ = true;
  full_wanted_ |= full;
  if (busy_) {
    return;
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&Impl::StartCycle, weak_factory_.GetWeakPtr()));
}

void BezelPasswordMirror::Impl::RequestCycleSoon() {
  if (!running_) {
    return;
  }
  debounce_timer_.Start(FROM_HERE, kLocalDebounce,
                        base::BindOnce(&Impl::RequestCycle,
                                       base::Unretained(this), false));
}

void BezelPasswordMirror::Impl::StartCycle() {
  if (!running_ || busy_ || !cycle_wanted_ || !client()) {
    return;
  }
  busy_ = true;
  cycle_wanted_ = false;
  cycle_ = std::make_unique<Cycle>();
  cycle_->full = full_wanted_ || cursor_.empty();
  full_wanted_ = false;
  store_->GetAllLogins(consumer_weak_factory_.GetWeakPtr());
}

void BezelPasswordMirror::Impl::OnGetPasswordStoreResultsOrErrorFrom(
    PasswordStoreInterface* store,
    base::expected<std::vector<StoredCredential>,
                   password_manager::PasswordStoreBackendError>
        results_or_error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!cycle_) {
    return;
  }
  if (!results_or_error.has_value()) {
    LOG(WARNING) << "Bezel: the login DB could not be read for CloudSync";
    EndCycle(false);
    return;
  }
  // Echoes of earlier local writes were delivered before this reply (the
  // store runs in order), so what is still expected never comes.
  expected_.clear();
  cycle_->locals = std::move(results_or_error.value());
  cycle_->renames = std::move(renames_);
  renames_.clear();
  cycle_->local_hash.resize(cycle_->locals.size());
  for (size_t i = 0; i < cycle_->locals.size(); ++i) {
    cycle_->local_index.emplace(ps::KeyOf(cycle_->locals[i]), i);
  }
  if (cycle_->full) {
    ListAll(std::string());
    return;
  }
  pb::ListChangesRequest request;
  request.set_cursor(cursor_);
  client()->ListChanges(request, base::BindOnce(&Impl::OnListChanges,
                                                weak_factory_.GetWeakPtr()));
}

void BezelPasswordMirror::Impl::ListAll(std::string page_token) {
  if (!client()) {
    EndCycle(false);
    return;
  }
  pb::ListLoginsRequest request;
  request.set_data(false);
  request.set_page_token(std::move(page_token));
  client()->ListLogins(request, base::BindOnce(&Impl::OnListPage,
                                               weak_factory_.GetWeakPtr()));
}

void BezelPasswordMirror::Impl::OnListPage(Status status,
                                           pb::ListLoginsReply reply) {
  if (!status.ok()) {
    OnReadFailed(status);
    return;
  }
  // The cursor of the first page: changes made while paging are listed again
  // next time, which is harmless.
  if (cycle_->new_cursor.empty()) {
    cycle_->new_cursor = reply.cursor();
  }
  for (const pb::Login& login : reply.logins()) {
    cycle_->remote_revs[login.id()] = login.revision();
  }
  if (!reply.next_page_token().empty()) {
    ListAll(reply.next_page_token());
    return;
  }
  PlanFetch();
}

void BezelPasswordMirror::Impl::OnListChanges(Status status,
                                              pb::ListChangesReply reply) {
  if (!status.ok()) {
    OnReadFailed(status);
    return;
  }
  if (reply.reset()) {
    cycle_->full = true;
    cycle_->new_cursor.clear();
    ListAll(std::string());
    return;
  }
  for (const pb::LoginChange& change : reply.changes()) {
    cycle_->changes[change.id()] = change;  // Oldest first: the last wins.
  }
  cycle_->new_cursor = reply.cursor();
  PlanFetch();
}

void BezelPasswordMirror::Impl::PlanFetch() {
  std::set<std::string> ids;
  // Known logins of other profiles: same revision, same owner.
  auto known_foreign = [this](const std::string& id,
                              const std::string& revision) {
    auto it = foreign_.find(id);
    return it != foreign_.end() && it->second == revision;
  };
  if (cycle_->full) {
    std::erase_if(foreign_, [this](const auto& item) {
      return !cycle_->remote_revs.contains(item.first);
    });
    for (const auto& [id, revision] : cycle_->remote_revs) {
      if (known_foreign(id, revision)) {
        continue;
      }
      auto it = entries_.find(id);
      if (it == entries_.end() || it->second.revision != revision ||
          it->second.shadow) {
        ids.insert(id);
      }
    }
  } else {
    for (const auto& [id, change] : cycle_->changes) {
      if (change.deleted()) {
        foreign_.erase(id);
        continue;
      }
      if (known_foreign(id, change.revision())) {
        continue;
      }
      auto it = entries_.find(id);
      if (it == entries_.end() || it->second.revision != change.revision()) {
        ids.insert(id);
      }
    }
    // Shadows whose key became free (and that the login DB can hold).
    for (const auto& [id, entry] : entries_) {
      if (entry.shadow && !by_key_.contains(entry.key) &&
          ps::StorableKey(entry.key)) {
        ids.insert(id);
      }
    }
  }
  // Logins changed here: their current extra (other apps' keys) is kept.
  for (auto& [id, entry] : entries_) {
    if (entry.shadow || ids.contains(id)) {
      continue;
    }
    if (cycle_->full ? !cycle_->remote_revs.contains(id)
                     : (cycle_->changes.contains(id) &&
                        cycle_->changes[id].deleted())) {
      continue;
    }
    size_t index = 0;
    const bool has_local = LocalFor(id, entry, &index) != nullptr;
    // Untagged ones of a profile that is no longer primary: check the owner.
    if ((has_local ? LocalChanged(entry, index) : !entry.pending) ||
        (entry.untagged && !primary_)) {
      ids.insert(id);
    }
  }
  cycle_->to_fetch.assign(ids.begin(), ids.end());
  cycle_->fetch_pos = 0;
  FetchNext();
}

void BezelPasswordMirror::Impl::FetchNext() {
  if (cycle_->fetch_pos >= cycle_->to_fetch.size()) {
    Reconcile();
    return;
  }
  if (!client()) {
    EndCycle(false);
    return;
  }
  pb::GetLoginsRequest request;
  const size_t end = std::min(cycle_->to_fetch.size(),
                              cycle_->fetch_pos + kIdsPerCall);
  for (size_t i = cycle_->fetch_pos; i < end; ++i) {
    request.add_ids(cycle_->to_fetch[i]);
  }
  cycle_->fetch_pos = end;
  client()->GetLogins(request, base::BindOnce(&Impl::OnFetched,
                                              weak_factory_.GetWeakPtr()));
}

void BezelPasswordMirror::Impl::OnFetched(Status status,
                                          pb::ListLoginsReply reply) {
  if (!status.ok()) {
    OnReadFailed(status);
    return;
  }
  for (pb::Login& login : *reply.mutable_logins()) {
    std::string id = login.id();
    cycle_->fetched[std::move(id)] = std::move(login);
  }
  FetchNext();
}

void BezelPasswordMirror::Impl::OnReadFailed(const Status& status) {
  if (IsFatal(status)) {
    StopForError(status);
    return;
  }
  VLOG(1) << "Bezel: CloudSync login read failed: " << status.ToString();
  retry_on_ready_ = true;
  EndCycle(false);
}

const StoredCredential* BezelPasswordMirror::Impl::LocalFor(
    const std::string& id,
    Entry& entry,
    size_t* index) {
  auto it = cycle_->local_index.find(entry.key);
  auto rename = cycle_->renames.find(id);
  if (it == cycle_->local_index.end() && rename != cycle_->renames.end()) {
    auto renamed = cycle_->local_index.find(rename->second);
    auto owner = by_key_.find(rename->second);
    if (renamed != cycle_->local_index.end() &&
        (owner == by_key_.end() || owner->second == id)) {
      it = renamed;
    }
  }
  if (it == cycle_->local_index.end()) {
    return nullptr;
  }
  *index = it->second;
  return &cycle_->locals[it->second];
}

const std::string& BezelPasswordMirror::Impl::LocalHash(size_t index) {
  std::string& hash = cycle_->local_hash[index];
  if (hash.empty()) {
    hash = ps::ContentHash(cycle_->locals[index]);
  }
  return hash;
}

bool BezelPasswordMirror::Impl::LocalChanged(const Entry& entry,
                                             size_t index) {
  const StoredCredential& local = cycle_->locals[index];
  return entry.pending || ps::KeyOf(local) != entry.key ||
         LocalHash(index) != entry.hash ||
         ps::ToMs(local.date_password_modified) != entry.password_ms;
}

void BezelPasswordMirror::Impl::Reconcile() {
  Cycle& cycle = *cycle_;

  // Whose is each fetched login? Others' are remembered and left alone.
  for (auto it = cycle.fetched.begin(); it != cycle.fetched.end();) {
    if (Owns(it->second)) {
      foreign_.erase(it->first);
      ++it;
      continue;
    }
    foreign_[it->first] = it->second.revision();
    cycle.not_ours.insert(it->first);
    it = cycle.fetched.erase(it);
  }

  // Ids to look at, in a stable order.
  std::set<std::string> ids;
  if (cycle.full) {
    for (const auto& [id, revision] : cycle.remote_revs) {
      ids.insert(id);
    }
    for (const auto& [id, entry] : entries_) {
      ids.insert(id);
    }
  } else {
    for (const auto& [id, change] : cycle.changes) {
      ids.insert(id);
    }
    for (const auto& [id, entry] : entries_) {
      ids.insert(id);  // Local changes and use counters.
    }
    for (const auto& [id, login] : cycle.fetched) {
      ids.insert(id);
    }
  }

  // Locals held by an entry are not new.
  for (auto& [id, entry] : entries_) {
    size_t index = 0;
    if (!entry.shadow && LocalFor(id, entry, &index)) {
      cycle.claimed.insert(index);
    }
  }

  for (const std::string& id : ids) {
    auto fetched = cycle.fetched.find(id);
    const pb::Login* remote =
        fetched == cycle.fetched.end() ? nullptr : &fetched->second;
    auto entry_it = entries_.find(id);

    bool remote_deleted = false;
    bool remote_changed = false;
    if (cycle.full) {
      auto rev = cycle.remote_revs.find(id);
      remote_deleted = rev == cycle.remote_revs.end();
      remote_changed = !remote_deleted &&
                       (entry_it == entries_.end() ||
                        entry_it->second.revision != rev->second);
    } else {
      auto change = cycle.changes.find(id);
      if (change != cycle.changes.end()) {
        remote_deleted = change->second.deleted();
        remote_changed = !remote_deleted &&
                         (entry_it == entries_.end() ||
                          entry_it->second.revision !=
                              change->second.revision());
      }
    }
    if (cycle.not_ours.contains(id)) {
      if (entry_it == entries_.end()) {
        continue;  // Another profile's: never added, never deleted.
      }
      // Ours until now (retagged, or untagged and no longer primary): gone
      // for this profile, as if deleted, but nothing is written to CloudSync.
      remote_deleted = true;
      remote_changed = false;
    }
    // Asked for but not returned: deleted meanwhile.
    if (remote_changed && !remote) {
      remote_deleted = true;
      remote_changed = false;
    }

    if (entry_it == entries_.end()) {
      if (remote && !remote_deleted) {
        HandleNewRemote(*remote);
      }
      continue;
    }
    Entry& entry = entry_it->second;
    if (entry.shadow) {
      if (remote_deleted) {
        DropEntry(id);
      } else if (remote) {
        // Changed, or its key may be free now.
        DropEntry(id);
        HandleNewRemote(*remote);
      }
      continue;
    }

    size_t index = 0;
    const StoredCredential* local = LocalFor(id, entry, &index);
    const bool local_changed = !local || LocalChanged(entry, index);

    if (remote_deleted) {
      if (local && !local_changed) {
        RemoveLocal(*local);  // Stays claimed: not written back.
      } else if (local) {
        // Changed here, deleted there: the change wins, as a new login.
        cycle.claimed.erase(index);
      }
      DropEntry(id);
      continue;
    }

    if (remote_changed) {
      if (!local) {
        // Deleted here, changed there: the change wins.
        ApplyRemote(id, *remote, nullptr);
      } else if (!local_changed) {
        ApplyRemote(id, *remote, local);
      } else {
        StoredCredential candidate = ps::CredentialFromLogin(*remote);
        ps::KeepLocalOnlyFields(*local, candidate);
        if (ps::SameContent(*local, candidate)) {
          SetEntry(id, remote->revision(), *local,
                   ps::ProfileTagOf(*remote).empty());
        } else if (ps::LocalWins(*local, entry.change_ms, *remote)) {
          QueuePut(id, remote->revision(), *local, remote);
        } else {
          ApplyRemote(id, *remote, local);
        }
      }
      continue;
    }

    // CloudSync did not change it.
    if (!local) {
      if (entry.pending) {
        // Deleted here, seen by the observer.
        if (!rejected_.contains(entry.key)) {
          QueueDelete(id, entry.revision);
        }
      } else if (remote) {
        // The local copy went missing without a deletion we saw (the DB
        // refused the write, or it was removed while the mirror was off):
        // never turn that into a deletion in CloudSync; restore it.
        ApplyRemote(id, *remote, nullptr);
      }
    } else if (local_changed) {
      if (!rejected_.contains(ps::KeyOf(*local))) {
        QueuePut(id, entry.revision, *local, remote);
      }
    } else if (ps::UseCountersAdvanced(*local, entry.used_ms,
                                       entry.times_used)) {
      QueueTouch(id, *local);
    }
  }

  // Logins only this device has.
  for (size_t i = 0; i < cycle.locals.size(); ++i) {
    if (cycle.claimed.contains(i)) {
      continue;
    }
    const ps::LoginKey key = ps::KeyOf(cycle.locals[i]);
    if (by_key_.contains(key) || rejected_.contains(key)) {
      continue;
    }
    QueuePut(std::string(), std::string(), cycle.locals[i], nullptr);
  }
  SaveSoon();
  ProcessNextOp();
}

void BezelPasswordMirror::Impl::HandleNewRemote(const pb::Login& remote) {
  Cycle& cycle = *cycle_;
  StoredCredential cred = ps::CredentialFromLogin(remote);
  const ps::LoginKey key = ps::KeyOf(cred);
  const bool untagged = ps::ProfileTagOf(remote).empty();

  auto owner = by_key_.find(key);
  if (!ps::StorableKey(key) ||
      (owner != by_key_.end() && owner->second != remote.id())) {
    SetShadow(remote.id(), remote.revision(), key);
    return;
  }
  auto local_it = cycle.local_index.find(key);
  if (local_it == cycle.local_index.end()) {
    AddLocal(cred);
    SetEntry(remote.id(), remote.revision(), cred, untagged);
    return;
  }
  const size_t index = local_it->second;
  if (cycle.claimed.contains(index)) {
    SetShadow(remote.id(), remote.revision(), key);
    return;
  }
  cycle.claimed.insert(index);
  const StoredCredential& local = cycle.locals[index];
  ps::KeepLocalOnlyFields(local, cred);
  if (ps::SameContent(local, cred)) {
    SetEntry(remote.id(), remote.revision(), local, untagged);
    return;
  }
  if (ps::LocalWins(local, /*local_change_ms=*/0, remote) &&
      !rejected_.contains(key)) {
    // Known from now on; the entry stays pending until the write lands.
    SetEntry(remote.id(), remote.revision(), local, untagged);
    Entry& entry = entries_[remote.id()];
    entry.pending = true;
    entry.hash.clear();
    // Not a change made here: an untagged login stays untagged.
    QueuePut(remote.id(), remote.revision(), local, &remote,
             /*keep_untagged=*/untagged);
    return;
  }
  UpdateLocal(cred);
  SetEntry(remote.id(), remote.revision(), cred, untagged);
}

void BezelPasswordMirror::Impl::ApplyRemote(const std::string& id,
                                            const pb::Login& remote,
                                            const StoredCredential* local) {
  Cycle& cycle = *cycle_;
  StoredCredential cred = ps::CredentialFromLogin(remote);
  if (local) {
    ps::KeepLocalOnlyFields(*local, cred);
  }
  const ps::LoginKey key = ps::KeyOf(cred);

  auto owner = by_key_.find(key);
  if (!ps::StorableKey(key) ||
      (owner != by_key_.end() && owner->second != id)) {
    // Moved onto the key of another login (or one the login DB refuses):
    // it is no longer ours to show.
    if (local) {
      RemoveLocal(*local);
    }
    SetShadow(id, remote.revision(), key);
    return;
  }
  if (local && ps::KeyOf(*local) == key) {
    UpdateLocal(cred);
  } else {
    if (local) {
      RemoveLocal(*local);
    }
    auto other = cycle.local_index.find(key);
    if (other != cycle.local_index.end()) {
      // An unsynced local copy with that key: CloudSync's version wins.
      cycle.claimed.insert(other->second);
      ps::KeepLocalOnlyFields(cycle.locals[other->second], cred);
      UpdateLocal(cred);
    } else {
      AddLocal(cred);
    }
  }
  SetEntry(id, remote.revision(), cred, ps::ProfileTagOf(remote).empty());
}

void BezelPasswordMirror::Impl::SetEntry(const std::string& id,
                                         const std::string& revision,
                                         const StoredCredential& cred,
                                         bool untagged) {
  foreign_.erase(id);
  Entry& entry = entries_[id];
  auto old = by_key_.find(entry.key);
  if (old != by_key_.end() && old->second == id) {
    by_key_.erase(old);
  }
  entry = Entry();
  entry.revision = revision;
  entry.key = ps::KeyOf(cred);
  entry.hash = ps::ContentHash(cred);
  entry.password_ms = ps::ToMs(cred.date_password_modified);
  entry.used_ms = ps::ToMs(cred.date_last_used);
  entry.times_used = cred.times_used_in_html_form;
  entry.untagged = untagged;
  by_key_[entry.key] = id;
}

void BezelPasswordMirror::Impl::SetShadow(const std::string& id,
                                          const std::string& revision,
                                          const ps::LoginKey& key) {
  foreign_.erase(id);
  Entry& entry = entries_[id];
  auto old = by_key_.find(entry.key);
  if (old != by_key_.end() && old->second == id) {
    by_key_.erase(old);
  }
  entry = Entry();
  entry.revision = revision;
  entry.key = key;
  entry.shadow = true;
}

void BezelPasswordMirror::Impl::DropEntry(const std::string& id) {
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    return;
  }
  auto owner = by_key_.find(it->second.key);
  if (owner != by_key_.end() && owner->second == id) {
    by_key_.erase(owner);
  }
  entries_.erase(it);
}

void BezelPasswordMirror::Impl::QueuePut(const std::string& id,
                                         const std::string& base,
                                         const StoredCredential& cred,
                                         const pb::Login* remote,
                                         bool keep_untagged) {
  Op op;
  op.kind = Op::Kind::kPut;
  op.id = id;
  op.base = base;
  op.keep_untagged = keep_untagged && primary_;
  op.cred = CloneStoredCredential(cred);
  if (remote) {
    op.remote = *remote;
  }
  cycle_->ops.push_back(std::move(op));
}

bool BezelPasswordMirror::Impl::Owns(const pb::Login& login) const {
  return ps::OwnsLogin(login, slot_, primary_);
}

void BezelPasswordMirror::Impl::QueueDelete(const std::string& id,
                                            const std::string& base) {
  Op op;
  op.kind = Op::Kind::kDelete;
  op.id = id;
  op.base = base;
  cycle_->ops.push_back(std::move(op));
}

void BezelPasswordMirror::Impl::QueueTouch(const std::string& id,
                                           const StoredCredential& cred) {
  Op op;
  op.kind = Op::Kind::kTouch;
  op.id = id;
  op.used_ms = ps::ToMs(cred.date_last_used);
  Entry& entry = entries_[id];
  entry.used_ms = op.used_ms;
  entry.times_used = cred.times_used_in_html_form;
  cycle_->ops.push_back(std::move(op));
}

void BezelPasswordMirror::Impl::AddLocal(const StoredCredential& cred) {
  expected_.emplace(ps::KeyOf(cred), ps::ContentHash(cred));
  StoredCredential copy = CloneStoredCredential(cred);
  copy.primary_key.reset();  // A new row.
  store_->AddLogin(std::move(copy));
}

void BezelPasswordMirror::Impl::UpdateLocal(const StoredCredential& cred) {
  expected_.emplace(ps::KeyOf(cred), ps::ContentHash(cred));
  store_->UpdateLogin(CloneStoredCredential(cred));
}

void BezelPasswordMirror::Impl::RemoveLocal(const StoredCredential& cred) {
  expected_.emplace(ps::KeyOf(cred), std::string());
  store_->RemoveLogin(FROM_HERE, cred);
}

// --- Remote writes ---------------------------------------------------------

void BezelPasswordMirror::Impl::ProcessNextOp() {
  if (cycle_->ops.empty()) {
    EndCycle(true);
    return;
  }
  cloudsync::ConnectorClient* connector = client();
  if (!connector) {
    EndCycle(true);
    return;
  }
  Op& op = cycle_->ops.front();
  switch (op.kind) {
    case Op::Kind::kPut: {
      pb::PutLoginRequest request;
      pb::Login* login = request.mutable_login();
      // Every login this profile writes carries its slot, except an
      // untagged one written back unchanged by the primary profile.
      *login = ps::LoginFromCredential(
          op.cred, op.keep_untagged ? std::string_view() : slot_);
      if (op.remote) {
        ps::KeepForeignExtra(*op.remote, *login);
      }
      login->set_id(op.id);
      request.set_base(op.base);
      connector->PutLogin(request, base::BindOnce(&Impl::OnPut,
                                                  weak_factory_.GetWeakPtr()));
      break;
    }
    case Op::Kind::kDelete: {
      pb::DeleteLoginRequest request;
      request.set_id(op.id);
      request.set_base(op.base);
      connector->DeleteLogin(
          request,
          base::BindOnce(&Impl::OnDelete, weak_factory_.GetWeakPtr()));
      break;
    }
    case Op::Kind::kTouch: {
      pb::TouchLoginRequest request;
      request.set_id(op.id);
      request.set_used_ms(op.used_ms);
      connector->TouchLogin(
          request, base::BindOnce(&Impl::OnTouch, weak_factory_.GetWeakPtr()));
      break;
    }
  }
}

void BezelPasswordMirror::Impl::OnPut(Status status, pb::Login login) {
  Op& op = cycle_->ops.front();
  if (status.ok()) {
    if (!op.id.empty() && op.id != login.id()) {
      DropEntry(op.id);
    }
    SetEntry(login.id(), login.revision(), op.cred, op.keep_untagged);
    cycle_->ops.pop_front();
    SaveSoon();
    ProcessNextOp();
    return;
  }
  if (!status.transport && !op.id.empty() &&
      (status.code == Code::kAborted || status.code == Code::kNotFound)) {
    if (status.code == Code::kNotFound || op.attempts >= kMaxAbortedRetries) {
      if (status.code == Code::kNotFound) {
        // Gone there: write the local version as a new login.
        DropEntry(op.id);
        op.id.clear();
        op.base.clear();
        op.remote.reset();
        ProcessNextOp();
        return;
      }
      // Keeps losing the race: leave it for the next cycle.
      cycle_->ops.pop_front();
      ProcessNextOp();
      return;
    }
    // Someone wrote meanwhile: fetch, re-apply the local change, retry.
    ++op.attempts;
    pb::GetLoginsRequest request;
    request.add_ids(op.id);
    client()->GetLogins(request, base::BindOnce(&Impl::OnRefetched,
                                                weak_factory_.GetWeakPtr()));
    return;
  }
  OnWriteFailed(status);
}

void BezelPasswordMirror::Impl::OnDelete(Status status,
                                         pb::DeleteLoginReply reply) {
  Op& op = cycle_->ops.front();
  if (status.ok() ||
      (!status.transport && status.code == Code::kNotFound)) {
    DropEntry(op.id);
    cycle_->ops.pop_front();
    SaveSoon();
    ProcessNextOp();
    return;
  }
  if (!status.transport && status.code == Code::kAborted &&
      op.attempts < kMaxAbortedRetries) {
    ++op.attempts;
    pb::GetLoginsRequest request;
    request.add_ids(op.id);
    client()->GetLogins(request, base::BindOnce(&Impl::OnRefetched,
                                                weak_factory_.GetWeakPtr()));
    return;
  }
  if (!status.transport && status.code == Code::kAborted) {
    cycle_->ops.pop_front();
    ProcessNextOp();
    return;
  }
  OnWriteFailed(status);
}

void BezelPasswordMirror::Impl::OnRefetched(Status status,
                                            pb::ListLoginsReply reply) {
  if (!status.ok()) {
    OnWriteFailed(status);
    return;
  }
  Op& op = cycle_->ops.front();
  const pb::Login* found = nullptr;
  for (const pb::Login& login : reply.logins()) {
    if (login.id() == op.id) {
      found = &login;
    }
  }
  if (!found) {
    if (op.kind == Op::Kind::kDelete) {
      DropEntry(op.id);
      cycle_->ops.pop_front();
    } else {
      DropEntry(op.id);
      op.id.clear();
      op.base.clear();
      op.remote.reset();
    }
    ProcessNextOp();
    return;
  }
  if (!Owns(*found)) {
    // Moved to another profile meanwhile: not ours to write any more.
    foreign_[op.id] = found->revision();
    DropEntry(op.id);
    cycle_->ops.pop_front();
    ProcessNextOp();
    return;
  }
  // The user's change is the latest: re-apply it on the new revision
  // (§ 12.3).
  op.base = found->revision();
  op.remote = *found;
  ProcessNextOp();
}

void BezelPasswordMirror::Impl::OnTouch(Status status,
                                        pb::TouchLoginReply reply) {
  if (IsFatal(status)) {
    StopForError(status);
    return;
  }
  cycle_->ops.pop_front();
  SaveSoon();
  ProcessNextOp();
}

void BezelPasswordMirror::Impl::OnWriteFailed(const Status& status) {
  if (IsFatal(status)) {
    StopForError(status);
    return;
  }
  Op& op = cycle_->ops.front();
  if (IsRetryable(status)) {
    // Offline or locked: the change stays pending (also in the file, so a
    // restart compares again) and is retried when CloudSync is READY.
    VLOG(1) << "Bezel: CloudSync login write pending: " << status.ToString();
    for (Op& pending : cycle_->ops) {
      auto it = entries_.find(pending.id);
      if (!pending.id.empty() && it != entries_.end() &&
          pending.kind != Op::Kind::kTouch) {
        it->second.pending = true;
      }
    }
    retry_on_ready_ = true;
    cycle_->ops.clear();
    EndCycle(true);
    return;
  }
  // INVALID_ARGUMENT, UNIMPLEMENTED, ...: retrying will not help.
  LOG(WARNING) << "Bezel: CloudSync refused a login write: "
               << status.ToString();
  if (op.kind == Op::Kind::kPut) {
    rejected_.insert(ps::KeyOf(op.cred));
  } else if (auto it = entries_.find(op.id); it != entries_.end()) {
    rejected_.insert(it->second.key);
  }
  cycle_->ops.pop_front();
  ProcessNextOp();
}

void BezelPasswordMirror::Impl::EndCycle(bool remote_applied) {
  if (remote_applied && cycle_ && !cycle_->new_cursor.empty()) {
    cursor_ = cycle_->new_cursor;
  }
  cycle_.reset();
  busy_ = false;
  SaveSoon();
  if (cycle_wanted_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&Impl::StartCycle, weak_factory_.GetWeakPtr()));
  }
}

// --- BezelPasswordMirror ---------------------------------------------------

BezelPasswordMirror::BezelPasswordMirror(
    Profile* profile,
    cloudsync::CloudSyncController* controller)
    : impl_(std::make_unique<Impl>(profile, controller)) {}

BezelPasswordMirror::~BezelPasswordMirror() = default;

void BezelPasswordMirror::Shutdown() {
  if (impl_) {
    impl_->Shutdown();
  }
}

}  // namespace bezel
