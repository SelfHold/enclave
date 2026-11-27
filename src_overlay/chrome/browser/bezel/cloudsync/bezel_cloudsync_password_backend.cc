// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_password_backend.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "base/barrier_closure.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_password_link.h"
#include "components/password_manager/core/browser/password_store/password_form_converters.h"
#include "components/password_manager/core/browser/password_store/password_store_backend_error.h"
#include "components/password_manager/core/browser/password_store/psl_matching_helper.h"

#if BUILDFLAG(IS_ANDROID)
#include "components/password_manager/core/browser/password_store/password_data_type_controller_delegate_android.h"
#endif

namespace bezel {

namespace {

namespace pb = cloudsync::pb;
namespace ps = password_sync;
using cloudsync::Code;
using cloudsync::Status;
using password_manager::FormPrimaryKey;
using password_manager::PasswordForm;
using password_manager::PasswordStoreBackendError;
using password_manager::PasswordStoreBackendErrorType;
using password_manager::PasswordStoreChange;
using password_manager::PasswordStoreChangeList;
using password_manager::StoredCredential;

// A failed load is tried again after this long (CloudSync not ready yet).
constexpr base::TimeDelta kRetryDelay = base::Seconds(30);

PasswordStoreBackendError WriteError() {
  return PasswordStoreBackendError(
      PasswordStoreBackendErrorType::kUncategorized);
}

StoredCredential Clone(const StoredCredential& cred) {
  return password_manager::CloneStoredCredential(cred);
}

std::vector<StoredCredential> CloneAll(
    const std::vector<const StoredCredential*>& creds) {
  std::vector<StoredCredential> out;
  out.reserve(creds.size());
  for (const StoredCredential* cred : creds) {
    out.push_back(Clone(*cred));
  }
  return out;
}

// The CloudSync id of a login was gone when we wrote it.
bool IsGone(const Status& status) {
  return !status.transport && status.code == Code::kNotFound;
}

bool IsConflict(const Status& status) {
  return !status.transport && status.code == Code::kAborted;
}

}  // namespace

CloudSyncPasswordBackend::Item::Item() = default;
CloudSyncPasswordBackend::Item::Item(Item&&) = default;
CloudSyncPasswordBackend::Item& CloudSyncPasswordBackend::Item::operator=(
    Item&&) = default;
CloudSyncPasswordBackend::Item::~Item() = default;

CloudSyncPasswordBackend::Op::Op() = default;
CloudSyncPasswordBackend::Op::Op(Op&&) = default;
CloudSyncPasswordBackend::Op& CloudSyncPasswordBackend::Op::operator=(Op&&) =
    default;
CloudSyncPasswordBackend::Op::~Op() = default;

CloudSyncPasswordBackend::CloudSyncPasswordBackend(base::FilePath profile_dir)
    : profile_dir_(std::move(profile_dir)) {}

CloudSyncPasswordBackend::~CloudSyncPasswordBackend() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!shut_down_ && !profile_dir_.empty()) {
    CloudSyncPasswordLink::SetBackend(profile_dir_, nullptr);
  }
}

// --- Connection ------------------------------------------------------------

void CloudSyncPasswordBackend::SetConnection(cloudsync::ConnectorClient* client,
                                             std::string slot,
                                             bool primary) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (shut_down_) {
    return;
  }
  if (!client) {
    slot.clear();
    primary = false;
  }
  if (client == client_ && slot == slot_ && primary == primary_) {
    return;
  }
  ++generation_;
  client_ = client;
  slot_ = std::move(slot);
  primary_ = primary;
  loaded_ = false;
  loading_ = false;
  refreshing_ = false;
  refresh_wanted_ = false;
  cursor_.clear();
  retry_timer_.Stop();
  FailAllOps();

  const bool had_logins = !items_.empty();
  ClearLogins();
  if (!usable()) {
    // Nothing to wait for any more: whoever waits gets the empty store.
    std::vector<base::OnceClosure> reads = std::move(waiting_reads_);
    for (base::OnceClosure& read : reads) {
      std::move(read).Run();
    }
    if (had_logins) {
      NotifyReload();
    }
    return;
  }
  StartLoad();
}

void CloudSyncPasswordBackend::OnRemoteLoginsChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!usable()) {
    return;
  }
  if (!loaded_ || loading_ || refreshing_) {
    refresh_wanted_ = true;
    return;
  }
  StartRefresh();
}

// --- PasswordStoreBackend: lifetime ------------------------------------------

void CloudSyncPasswordBackend::InitBackend(
    RemoteChangesReceived remote_form_changes_received,
    base::RepeatingClosure sync_enabled_or_disabled_cb,
    base::OnceCallback<void(bool)> completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  remote_changes_ = std::move(remote_form_changes_received);
  if (!profile_dir_.empty()) {
    // May call SetConnection right away.
    CloudSyncPasswordLink::SetBackend(profile_dir_, this);
  }
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(completion), /*success=*/true));
}

void CloudSyncPasswordBackend::Shutdown(base::OnceClosure shutdown_completed) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!shut_down_ && !profile_dir_.empty()) {
    CloudSyncPasswordLink::SetBackend(profile_dir_, nullptr);
  }
  shut_down_ = true;
  weak_ptr_factory_.InvalidateWeakPtrs();
  retry_timer_.Stop();
  client_ = nullptr;
  // The store is going away: nobody is left to hear replies.
  ops_.clear();
  op_running_ = false;
  waiting_reads_.clear();
  remote_changes_.Reset();
  ClearLogins();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, std::move(shutdown_completed));
}

password_manager::ActionableError CloudSyncPasswordBackend::GetError() {
  return password_manager::ActionableError::kNoError;
}

// --- Reads -------------------------------------------------------------------

void CloudSyncPasswordBackend::GetAllLoginsAsync(
    password_manager::BackendLoginsOrErrorReply callback) {
  Read(base::BindOnce(&CloudSyncPasswordBackend::AllLogins,
                      base::Unretained(this), /*autofillable_only=*/false),
       std::move(callback));
}

void CloudSyncPasswordBackend::GetAllLoginsWithAffiliationAndBrandingAsync(
    password_manager::BackendLoginsOrErrorReply callback) {
  // No affiliation service on Android: the plain list.
  GetAllLoginsAsync(std::move(callback));
}

void CloudSyncPasswordBackend::GetAutofillableLoginsAsync(
    password_manager::BackendLoginsOrErrorReply callback) {
  Read(base::BindOnce(&CloudSyncPasswordBackend::AllLogins,
                      base::Unretained(this), /*autofillable_only=*/true),
       std::move(callback));
}

void CloudSyncPasswordBackend::FillMatchingLoginsAsync(
    password_manager::BackendLoginsOrErrorReply callback,
    bool include_psl,
    const std::vector<password_manager::PasswordFormDigest>& forms) {
  Read(base::BindOnce(
           [](const CloudSyncPasswordBackend* self,
              const std::vector<password_manager::PasswordFormDigest>& forms,
              bool include_psl) {
             std::vector<StoredCredential> out;
             for (const auto& form : forms) {
               std::vector<StoredCredential> matched = self->MatchingLogins(
                   form, include_psl, /*set_match_type=*/false);
               std::move(matched.begin(), matched.end(),
                         std::back_inserter(out));
             }
             return out;
           },
           base::Unretained(this), forms, include_psl),
       std::move(callback));
}

void CloudSyncPasswordBackend::GetGroupedMatchingLoginsAsync(
    const password_manager::PasswordFormDigest& form_digest,
    password_manager::BackendLoginsOrErrorReply callback) {
  // Exact, PSL and federated matches; no affiliations or groups (they come
  // from Google's affiliation service, which this build does not use).
  Read(base::BindOnce(&CloudSyncPasswordBackend::MatchingLogins,
                      base::Unretained(this), form_digest,
                      /*include_psl=*/true, /*set_match_type=*/true),
       std::move(callback));
}

void CloudSyncPasswordBackend::Read(
    base::OnceCallback<std::vector<StoredCredential>()> compute,
    password_manager::BackendLoginsOrErrorReply callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Unretained: the closure runs on this sequence, and `waiting_reads_` is
  // cleared in Shutdown; the posted reply only carries the result.
  base::OnceClosure answer = base::BindOnce(
      [](base::OnceCallback<std::vector<StoredCredential>()> compute,
         password_manager::BackendLoginsOrErrorReply callback) {
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE,
            base::BindOnce(std::move(callback), std::move(compute).Run()));
      },
      std::move(compute), std::move(callback));
  if (usable() && !loaded_) {
    waiting_reads_.push_back(std::move(answer));  // Answered by FinishLoad.
    return;
  }
  std::move(answer).Run();
}

std::vector<StoredCredential> CloudSyncPasswordBackend::AllLogins(
    bool autofillable_only) const {
  std::vector<const StoredCredential*> found;
  for (const auto& [key, id] : by_key_) {
    const StoredCredential& cred = items_.at(id).cred;
    if (!autofillable_only || !cred.blocked_by_user) {
      found.push_back(&cred);
    }
  }
  return CloneAll(found);
}

std::vector<StoredCredential> CloudSyncPasswordBackend::MatchingLogins(
    const password_manager::PasswordFormDigest& form,
    bool include_psl,
    bool set_match_type) const {
  std::vector<StoredCredential> out;
  for (const auto& [key, id] : by_key_) {
    const StoredCredential& cred = items_.at(id).cred;
    std::optional<affiliations::MatchType> type;
    switch (password_manager::GetMatchResult(cred, form)) {
      case password_manager::MatchResult::NO_MATCH:
        continue;
      case password_manager::MatchResult::EXACT_MATCH:
      case password_manager::MatchResult::FEDERATED_MATCH:
        type = affiliations::MatchType::kExact;
        break;
      case password_manager::MatchResult::PSL_MATCH:
      case password_manager::MatchResult::FEDERATED_PSL_MATCH:
        if (!include_psl) {
          continue;
        }
        type = affiliations::MatchType::kPSL;
        break;
    }
    StoredCredential copy = Clone(cred);
    if (set_match_type) {
      copy.match_type = type;
    }
    out.push_back(std::move(copy));
  }
  return out;
}

// --- Full load ---------------------------------------------------------------

void CloudSyncPasswordBackend::StartLoad() {
  if (!usable() || loading_) {
    return;
  }
  loading_ = true;
  pb::ListLoginsRequest request;
  request.set_data(true);
  client_->ListLogins(
      request,
      base::BindOnce(&CloudSyncPasswordBackend::OnLoadPage,
                     weak_ptr_factory_.GetWeakPtr(), generation_,
                     std::map<std::string, pb::Login>(), std::string()));
}

void CloudSyncPasswordBackend::OnLoadPage(
    int generation,
    std::map<std::string, pb::Login> collected,
    std::string cursor,
    Status status,
    pb::ListLoginsReply reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  if (!status.ok()) {
    LOG(WARNING) << "Bezel: CloudSync logins could not be read: "
                 << status.ToString();
    loading_ = false;
    // Reads do not wait for a retry: they get what there is (nothing yet).
    std::vector<base::OnceClosure> reads = std::move(waiting_reads_);
    for (base::OnceClosure& read : reads) {
      std::move(read).Run();
    }
    RetryLoadLater();
    return;
  }
  // The cursor of the first page: changes made while paging are listed
  // again by the next ListChanges.
  if (cursor.empty()) {
    cursor = reply.cursor();
  }
  for (pb::Login& login : *reply.mutable_logins()) {
    std::string id = login.id();
    collected[id] = std::move(login);
  }
  if (!reply.next_page_token().empty()) {
    pb::ListLoginsRequest request;
    request.set_data(true);
    request.set_page_token(reply.next_page_token());
    client_->ListLogins(
        request, base::BindOnce(&CloudSyncPasswordBackend::OnLoadPage,
                                weak_ptr_factory_.GetWeakPtr(), generation,
                                std::move(collected), std::move(cursor)));
    return;
  }
  FinishLoad(std::move(collected), std::move(cursor));
}

void CloudSyncPasswordBackend::FinishLoad(
    std::map<std::string, pb::Login> logins,
    std::string cursor) {
  const bool was_loaded = loaded_;
  ClearLogins();
  for (auto& [id, login] : logins) {
    if (Owns(login)) {
      StoredCredential cred = CredentialFor(login, id);
      SetLogin(login, std::move(cred));
    } else {
      foreign_[id] = login.revision();
    }
  }
  cursor_ = std::move(cursor);
  loading_ = false;
  loaded_ = true;

  std::vector<base::OnceClosure> reads = std::move(waiting_reads_);
  for (base::OnceClosure& read : reads) {
    std::move(read).Run();
  }
  // The store reads everything again (also after a reset of the cursor).
  if (was_loaded || !items_.empty()) {
    NotifyReload();
  }
  RunNextOp();
  if (refresh_wanted_) {
    refresh_wanted_ = false;
    StartRefresh();
  }
}

void CloudSyncPasswordBackend::RetryLoadLater() {
  retry_timer_.Start(FROM_HERE, kRetryDelay,
                     base::BindOnce(&CloudSyncPasswordBackend::StartLoad,
                                    base::Unretained(this)));
}

// --- Incremental refresh -----------------------------------------------------

void CloudSyncPasswordBackend::StartRefresh() {
  if (cursor_.empty()) {
    StartLoad();
    return;
  }
  refreshing_ = true;
  pb::ListChangesRequest request;
  request.set_cursor(cursor_);
  client_->ListChanges(
      request, base::BindOnce(&CloudSyncPasswordBackend::OnChanges,
                              weak_ptr_factory_.GetWeakPtr(), generation_));
}

void CloudSyncPasswordBackend::OnChanges(int generation,
                                         Status status,
                                         pb::ListChangesReply reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  if (!status.ok()) {
    LOG(WARNING) << "Bezel: CloudSync login changes could not be read: "
                 << status.ToString();
    EndRefresh();
    return;
  }
  if (reply.reset()) {
    refreshing_ = false;
    StartLoad();
    return;
  }
  // The last change of an id wins (oldest first).
  std::map<std::string, pb::LoginChange> changes;
  for (const pb::LoginChange& change : reply.changes()) {
    changes[change.id()] = change;
  }
  std::vector<std::string> deleted;
  pb::GetLoginsRequest request;
  for (const auto& [id, change] : changes) {
    if (change.deleted()) {
      deleted.push_back(id);
      continue;
    }
    auto item = items_.find(id);
    if (item != items_.end() &&
        item->second.login.revision() == change.revision()) {
      continue;  // Our own write, or nothing new.
    }
    auto foreign = foreign_.find(id);
    if (foreign != foreign_.end() && foreign->second == change.revision()) {
      continue;
    }
    request.add_ids(id);
  }
  if (request.ids().empty()) {
    OnChangedLogins(generation, std::move(deleted), reply.cursor(),
                    Status::Ok(), pb::ListLoginsReply());
    return;
  }
  client_->GetLogins(request,
                     base::BindOnce(&CloudSyncPasswordBackend::OnChangedLogins,
                                    weak_ptr_factory_.GetWeakPtr(), generation,
                                    std::move(deleted), reply.cursor()));
}

void CloudSyncPasswordBackend::OnChangedLogins(int generation,
                                               std::vector<std::string> deleted,
                                               std::string cursor,
                                               Status status,
                                               pb::ListLoginsReply reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  if (!status.ok()) {
    LOG(WARNING) << "Bezel: changed CloudSync logins could not be read: "
                 << status.ToString();
    EndRefresh();  // The cursor stays: the same changes come again.
    return;
  }
  PasswordStoreChangeList changes;
  auto append = [&changes](PasswordStoreChangeList more) {
    std::move(more.begin(), more.end(), std::back_inserter(changes));
  };
  for (const std::string& id : deleted) {
    foreign_.erase(id);
    append(DropLogin(id));
  }
  for (const pb::Login& login : reply.logins()) {
    if (Owns(login)) {
      foreign_.erase(login.id());
      StoredCredential cred = CredentialFor(login, login.id());
      append(SetLogin(login, std::move(cred)));
    } else {
      // Another profile's now (or still): gone from this one.
      foreign_[login.id()] = login.revision();
      append(DropLogin(login.id()));
    }
  }
  cursor_ = std::move(cursor);
  NotifyChanges(std::move(changes));
  EndRefresh();
}

void CloudSyncPasswordBackend::EndRefresh() {
  refreshing_ = false;
  if (refresh_wanted_ && usable() && loaded_) {
    refresh_wanted_ = false;
    StartRefresh();
  }
}

// --- State -------------------------------------------------------------------

bool CloudSyncPasswordBackend::Owns(const pb::Login& login) const {
  return ps::OwnsLogin(login, slot_, primary_);
}

StoredCredential CloudSyncPasswordBackend::CredentialFor(
    const pb::Login& login,
    const std::string& id) const {
  StoredCredential cred = ps::CredentialFromLogin(login);
  auto old = items_.find(id);
  if (old != items_.end()) {
    // Form data and the like that CloudSync does not carry.
    ps::KeepLocalOnlyFields(old->second.cred, cred);
  }
  cred.in_store = PasswordForm::Store::kProfileStore;
  return cred;
}

FormPrimaryKey CloudSyncPasswordBackend::PrimaryKeyFor(const std::string& id) {
  auto [it, inserted] = primary_keys_.try_emplace(id, next_primary_key_);
  if (inserted) {
    ++next_primary_key_;
  }
  return FormPrimaryKey(it->second);
}

PasswordStoreChangeList CloudSyncPasswordBackend::SetLogin(
    const pb::Login& login,
    StoredCredential cred) {
  PasswordStoreChangeList changes;
  const std::string& id = login.id();
  cred.primary_key = PrimaryKeyFor(id);
  cred.in_store = PasswordForm::Store::kProfileStore;
  const ps::LoginKey key = ps::KeyOf(cred);

  auto old = items_.find(id);
  if (old != items_.end()) {
    const ps::LoginKey old_key = ps::KeyOf(old->second.cred);
    auto visible = by_key_.find(old_key);
    const bool was_visible = visible != by_key_.end() && visible->second == id;
    if (was_visible && old_key == key) {
      const bool password_changed =
          old->second.cred.password_value != cred.password_value;
      changes.emplace_back(PasswordStoreChange::UPDATE, Clone(cred),
                           password_changed);
      old->second.login = login;
      old->second.cred = std::move(cred);
      return changes;
    }
    // Its key changed (or it was hidden): take it away first.
    std::ranges::move(DropLogin(id), std::back_inserter(changes));
  }

  Item item;
  item.login = login;
  item.cred = std::move(cred);
  const bool storable = ps::StorableKey(key);
  const bool free_key = !by_key_.contains(key);
  if (storable && free_key) {
    by_key_[key] = id;
    changes.emplace_back(PasswordStoreChange::ADD, Clone(item.cred));
  }
  // Otherwise it is kept hidden: the password manager holds one login per
  // key, and another CloudSync entry has this one (a conflict copy).
  items_[id] = std::move(item);
  return changes;
}

PasswordStoreChangeList CloudSyncPasswordBackend::DropLogin(
    const std::string& id) {
  PasswordStoreChangeList changes;
  auto it = items_.find(id);
  if (it == items_.end()) {
    return changes;
  }
  Item item = std::move(it->second);
  items_.erase(it);  // Its primary key stays reserved for the id.
  const ps::LoginKey key = ps::KeyOf(item.cred);
  auto visible = by_key_.find(key);
  if (visible == by_key_.end() || visible->second != id) {
    return changes;  // It was hidden.
  }
  by_key_.erase(visible);
  changes.emplace_back(PasswordStoreChange::REMOVE, std::move(item.cred));
  // A hidden login with the same key shows up in its place.
  for (auto& [other_id, other] : items_) {
    if (ps::KeyOf(other.cred) == key) {
      by_key_[key] = other_id;
      changes.emplace_back(PasswordStoreChange::ADD, Clone(other.cred));
      break;
    }
  }
  return changes;
}

void CloudSyncPasswordBackend::ClearLogins() {
  items_.clear();
  by_key_.clear();
  foreign_.clear();
  // Primary keys stay: the same login keeps its key across reloads.
}

const CloudSyncPasswordBackend::Item* CloudSyncPasswordBackend::FindByKey(
    const StoredCredential& cred) const {
  auto it = by_key_.find(ps::KeyOf(cred));
  return it == by_key_.end() ? nullptr : &items_.at(it->second);
}

void CloudSyncPasswordBackend::NotifyChanges(PasswordStoreChangeList changes) {
  if (changes.empty() || remote_changes_.is_null()) {
    return;
  }
  remote_changes_.Run(std::move(changes));
}

void CloudSyncPasswordBackend::NotifyReload() {
  if (remote_changes_.is_null()) {
    return;
  }
  // nullopt: the store reads every login again (OnLoginsRetained).
  remote_changes_.Run(std::optional<PasswordStoreChangeList>());
}

// --- Writes ------------------------------------------------------------------

void CloudSyncPasswordBackend::AddLoginAsync(StoredCredential cred,
                                             ChangesReply callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!usable()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), base::unexpected(WriteError())));
    return;
  }
  Op op;
  op.kind = Op::Kind::kPut;  // Over a login with the same key: replaces it.
  op.cred = std::move(cred);
  WriteThenReply(std::move(op), std::move(callback));
}

void CloudSyncPasswordBackend::UpdateLoginAsync(StoredCredential cred,
                                                ChangesReply callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!usable()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), base::unexpected(WriteError())));
    return;
  }
  Op op;
  op.kind = Op::Kind::kPut;
  op.cred = std::move(cred);
  op.update = true;  // The id is looked up when the op runs.
  WriteThenReply(std::move(op), std::move(callback));
}

void CloudSyncPasswordBackend::RemoveLoginAsync(const base::Location& location,
                                                StoredCredential cred,
                                                ChangesReply callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!usable()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), base::unexpected(WriteError())));
    return;
  }
  Op op;
  op.kind = Op::Kind::kDelete;
  op.cred = std::move(cred);
  WriteThenReply(std::move(op), std::move(callback));
}

void CloudSyncPasswordBackend::RemoveLoginsCreatedBetweenAsync(
    const base::Location& location,
    base::Time delete_begin,
    base::Time delete_end,
    ChangesReply callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!usable() || !loaded_) {
    // "Delete browsing data" never deletes CloudSync's passwords it cannot
    // see; nothing of this profile is known yet.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), PasswordStoreChangeList()));
    return;
  }
  std::vector<StoredCredential> victims;
  for (const auto& [key, id] : by_key_) {
    const StoredCredential& cred = items_.at(id).cred;
    if (cred.date_created >= delete_begin &&
        (delete_end.is_null() || cred.date_created < delete_end)) {
      victims.push_back(Clone(cred));
    }
  }
  if (victims.empty()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), PasswordStoreChangeList()));
    return;
  }
  // Every delete is queued now; the reply collects what was removed.
  struct Collector {
    PasswordStoreChangeList changes;
    bool failed = false;
  };
  auto collector = std::make_shared<Collector>();
  base::RepeatingClosure barrier = base::BarrierClosure(
      victims.size(),
      base::BindOnce(
          [](std::shared_ptr<Collector> collector, ChangesReply callback) {
            if (collector->failed && collector->changes.empty()) {
              std::move(callback).Run(base::unexpected(WriteError()));
              return;
            }
            std::move(callback).Run(std::move(collector->changes));
          },
          collector, std::move(callback)));
  for (StoredCredential& victim : victims) {
    Op op;
    op.kind = Op::Kind::kDelete;
    op.cred = std::move(victim);
    op.done = base::BindOnce(
        [](std::shared_ptr<Collector> collector, base::RepeatingClosure barrier,
           std::optional<PasswordStoreChangeList> result) {
          if (result) {
            std::ranges::move(*result, std::back_inserter(collector->changes));
          } else {
            collector->failed = true;
          }
          barrier.Run();
        },
        collector, barrier);
    Enqueue(std::move(op));
  }
}

void CloudSyncPasswordBackend::DisableAutoSignInForOriginsAsync(
    const base::RepeatingCallback<bool(const GURL&)>& origin_filter,
    base::OnceClosure completion) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<StoredCredential> targets;
  if (usable() && loaded_) {
    for (const auto& [key, id] : by_key_) {
      const StoredCredential& cred = items_.at(id).cred;
      if (!cred.skip_zero_click && origin_filter.Run(cred.url)) {
        StoredCredential copy = Clone(cred);
        copy.skip_zero_click = true;
        targets.push_back(std::move(copy));
      }
    }
  }
  if (targets.empty()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(completion));
    return;
  }
  base::RepeatingClosure barrier =
      base::BarrierClosure(targets.size(), std::move(completion));
  for (StoredCredential& target : targets) {
    Op op;
    op.kind = Op::Kind::kPut;
    op.update = true;
    op.cred = std::move(target);
    op.done = base::BindOnce(
        [](base::RepeatingClosure barrier,
           std::optional<PasswordStoreChangeList>) { barrier.Run(); },
        barrier);
    Enqueue(std::move(op));
  }
}

void CloudSyncPasswordBackend::WriteThenReply(Op op, ChangesReply callback) {
  op.done = base::BindOnce(
      [](ChangesReply callback, std::optional<PasswordStoreChangeList> result) {
        if (!result) {
          std::move(callback).Run(base::unexpected(WriteError()));
          return;
        }
        std::move(callback).Run(std::move(*result));
      },
      std::move(callback));
  Enqueue(std::move(op));
}

void CloudSyncPasswordBackend::Enqueue(Op op) {
  ops_.push_back(std::move(op));
  RunNextOp();
}

void CloudSyncPasswordBackend::RunNextOp() {
  // Writes wait for the load: a CAS needs the current revision.
  while (!op_running_ && usable() && loaded_ && !ops_.empty()) {
    Op& op = ops_.front();
    const Item* existing = nullptr;
    if (op.id.empty()) {
      existing = FindByKey(op.cred);
    } else if (auto it = items_.find(op.id); it != items_.end()) {
      existing = &it->second;
    }

    if (op.kind == Op::Kind::kDelete) {
      if (!existing) {
        FinishOp(PasswordStoreChangeList());  // Not there: nothing removed.
        continue;
      }
      op.id = existing->login.id();
      pb::DeleteLoginRequest request;
      request.set_id(op.id);
      request.set_base(existing->login.revision());
      op_running_ = true;
      client_->DeleteLogin(
          request, base::BindOnce(&CloudSyncPasswordBackend::OnDeleteDone,
                                  weak_ptr_factory_.GetWeakPtr(), generation_));
      return;
    }

    // kPut.
    if (op.update) {
      if (!existing) {
        // UpdateLogin of an unknown login changes nothing (as the login DB).
        FinishOp(PasswordStoreChangeList());
        continue;
      }
      op.id = existing->login.id();
      if (ps::SameContent(existing->cred, op.cred)) {
        // Only what CloudSync does not version moved (use counters, form
        // data): no new revision (§ 12.4).
        const bool used = ps::UseCountersAdvanced(
            op.cred, ps::ToMs(existing->cred.date_last_used),
            existing->cred.times_used_in_html_form);
        const pb::Login login = existing->login;  // SetLogin replaces it.
        PasswordStoreChangeList changes = SetLogin(login, Clone(op.cred));
        if (used) {
          pb::TouchLoginRequest request;
          request.set_id(op.id);
          request.set_used_ms(ps::ToMs(op.cred.date_last_used));
          // Fire and forget: a lost touch only loses a use counter.
          client_->TouchLogin(
              request, base::BindOnce([](Status, pb::TouchLoginReply) {}));
        }
        FinishOp(std::move(changes));
        continue;
      }
    }
    pb::PutLoginRequest request;
    *request.mutable_login() = ps::LoginFromCredential(op.cred, slot_);
    if (existing) {
      ps::KeepForeignExtra(existing->login, *request.mutable_login());
      request.mutable_login()->set_id(existing->login.id());
      request.set_base(existing->login.revision());
      // CloudSync keeps the original creation time.
      request.mutable_login()->set_created_ms(existing->login.created_ms());
      op.id = existing->login.id();
      op.replace = !op.update;  // AddLogin over an existing key.
    } else {
      op.id.clear();
      op.replace = false;
    }
    op_running_ = true;
    client_->PutLogin(
        request, base::BindOnce(&CloudSyncPasswordBackend::OnPutDone,
                                weak_ptr_factory_.GetWeakPtr(), generation_));
    return;
  }
}

void CloudSyncPasswordBackend::OnPutDone(int generation,
                                         Status status,
                                         pb::Login login) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  op_running_ = false;
  Op& op = ops_.front();
  if ((IsConflict(status) || IsGone(status)) && !op.retried) {
    // Changed or deleted elsewhere since we read it: read it again and
    // retry once on top of it (the user's change here wins).
    op.retried = true;
    pb::GetLoginsRequest request;
    request.add_ids(op.id);
    op_running_ = true;
    client_->GetLogins(
        request, base::BindOnce(&CloudSyncPasswordBackend::OnConflictReread,
                                weak_ptr_factory_.GetWeakPtr(), generation_));
    return;
  }
  if (!status.ok()) {
    LOG(WARNING) << "Bezel: a login could not be written to CloudSync: "
                 << status.ToString();
    FinishOp(std::nullopt);
    return;
  }
  // The password manager's own credential (CloudSync may not echo the
  // local-only fields), with what CloudSync decided about times.
  StoredCredential cred = Clone(op.cred);
  PasswordStoreChangeList changes;
  if (op.replace) {
    const Item* old = FindByKey(cred);
    if (old) {
      changes.emplace_back(PasswordStoreChange::REMOVE, Clone(old->cred));
    }
    PasswordStoreChangeList set = SetLogin(login, std::move(cred));
    for (PasswordStoreChange& change : set) {
      if (change.type() == PasswordStoreChange::UPDATE) {
        changes.emplace_back(PasswordStoreChange::ADD,
                             Clone(change.credential()));
      } else {
        changes.push_back(std::move(change));
      }
    }
  } else {
    changes = SetLogin(login, std::move(cred));
  }
  foreign_.erase(login.id());
  FinishOp(std::move(changes));
}

void CloudSyncPasswordBackend::OnDeleteDone(int generation,
                                            Status status,
                                            pb::DeleteLoginReply reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  op_running_ = false;
  Op& op = ops_.front();
  if (IsConflict(status) && !op.retried) {
    op.retried = true;
    pb::GetLoginsRequest request;
    request.add_ids(op.id);
    op_running_ = true;
    client_->GetLogins(
        request, base::BindOnce(&CloudSyncPasswordBackend::OnConflictReread,
                                weak_ptr_factory_.GetWeakPtr(), generation_));
    return;
  }
  if (!status.ok() && !IsGone(status)) {
    LOG(WARNING) << "Bezel: a login could not be deleted in CloudSync: "
                 << status.ToString();
    FinishOp(std::nullopt);
    return;
  }
  FinishOp(DropLogin(op.id));
}

void CloudSyncPasswordBackend::OnConflictReread(int generation,
                                                Status status,
                                                pb::ListLoginsReply reply) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;
  }
  op_running_ = false;
  if (!status.ok()) {
    FinishOp(std::nullopt);
    return;
  }
  Op& op = ops_.front();
  // What changed there is news for the password manager too.
  PasswordStoreChangeList changes;
  if (reply.logins().empty()) {
    changes = DropLogin(op.id);  // Deleted there.
  } else if (Owns(reply.logins(0))) {
    const pb::Login& login = reply.logins(0);
    changes = SetLogin(login, CredentialFor(login, login.id()));
  } else {
    foreign_[op.id] = reply.logins(0).revision();
    changes = DropLogin(op.id);
  }
  NotifyChanges(std::move(changes));

  if (op.kind == Op::Kind::kDelete && !items_.contains(op.id)) {
    FinishOp(PasswordStoreChangeList());  // Already gone.
    return;
  }
  // Run it again against the current state; a put of a login deleted there
  // becomes a new login.
  if (!items_.contains(op.id)) {
    op.id.clear();
    op.update = false;
    op.replace = false;
  }
  RunNextOp();
}

void CloudSyncPasswordBackend::FinishOp(
    std::optional<PasswordStoreChangeList> result) {
  Op op = std::move(ops_.front());
  ops_.pop_front();
  if (op.done) {
    std::move(op.done).Run(std::move(result));
  }
}

void CloudSyncPasswordBackend::FailAllOps() {
  base::circular_deque<Op> ops = std::move(ops_);
  ops_.clear();
  op_running_ = false;
  for (Op& op : ops) {
    if (op.done) {
      std::move(op.done).Run(std::nullopt);
    }
  }
}

// --- The rest ----------------------------------------------------------------

password_manager::SmartBubbleStatsStore*
CloudSyncPasswordBackend::GetSmartBubbleStatsStore() {
  return nullptr;
}

std::unique_ptr<syncer::DataTypeControllerDelegate>
CloudSyncPasswordBackend::CreateSyncControllerDelegate() {
  // PASSWORDS never syncs through Chromium's engine in this build (patch
  // 0009 turns it off for the local backend).
#if BUILDFLAG(IS_ANDROID)
  return std::make_unique<
      password_manager::PasswordDataTypeControllerDelegateAndroid>();
#else
  return nullptr;
#endif
}

void CloudSyncPasswordBackend::OnSyncServiceInitialized(
    syncer::SyncService* sync_service) {}

base::WeakPtr<password_manager::PasswordStoreBackend>
CloudSyncPasswordBackend::AsWeakPtr() {
  return weak_ptr_factory_.GetWeakPtr();
}

}  // namespace bezel
