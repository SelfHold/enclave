// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_PASSWORD_BACKEND_H_
#define CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_PASSWORD_BACKEND_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "chrome/browser/enclave/shard/enclave_password_conversion.h"
#include "components/enclave_shard/connector_client.h"
#include "components/password_manager/core/browser/password_store/password_store_backend.h"

namespace enclave {

// The profile password store on Android (docs/SHARD.md, "Passwords on
// Android"). Chromium 158 has no local login DB there, so the logins live in
// Shard's password store only (CONNECTOR.md § 12): this backend keeps the
// profile's logins in memory, answers reads from them, and writes every change
// straight to Shard. Shard keeps its own copy on the device and serves
// reads while offline, so there is no second copy of the secrets on disk.
//
// Logins carry extra["enclave.profile"] = slot; a profile sees its own logins,
// and the primary profile also the untagged ones (SHARD.md, "Browser
// profiles"). Without a usable connection (sync off, no scope, Shard
// locked or missing) the store is empty and refuses writes.
//
// The connection comes from ShardPasswordLink, or from a test through
// SetConnection / OnRemoteLoginsChanged. Main sequence only.
class ShardPasswordBackend : public password_manager::PasswordStoreBackend {
 public:
  // `profile_dir` names the profile for ShardPasswordLink; an empty path
  // leaves the backend unlinked (tests).
  explicit ShardPasswordBackend(base::FilePath profile_dir);
  ShardPasswordBackend(const ShardPasswordBackend&) = delete;
  ShardPasswordBackend& operator=(const ShardPasswordBackend&) = delete;
  ~ShardPasswordBackend() override;

  // `client` null: no usable connection. Otherwise the logins of `slot`
  // (`primary`: plus the untagged ones) are loaded again. `client` must
  // outlive the next call of SetConnection or Shutdown.
  void SetConnection(shard::ConnectorClient* client,
                     std::string slot,
                     bool primary);
  // Watch reported a LOGIN change.
  void OnRemoteLoginsChanged();

  bool loaded_for_testing() const { return loaded_; }

  // PasswordStoreBackend:
  void InitBackend(RemoteChangesReceived remote_form_changes_received,
                   base::RepeatingClosure sync_enabled_or_disabled_cb,
                   base::OnceCallback<void(bool)> completion) override;
  void Shutdown(base::OnceClosure shutdown_completed) override;
  password_manager::ActionableError GetError() override;
  void GetAllLoginsAsync(
      password_manager::BackendLoginsOrErrorReply callback) override;
  void GetAllLoginsWithAffiliationAndBrandingAsync(
      password_manager::BackendLoginsOrErrorReply callback) override;
  void GetAutofillableLoginsAsync(
      password_manager::BackendLoginsOrErrorReply callback) override;
  void FillMatchingLoginsAsync(
      password_manager::BackendLoginsOrErrorReply callback,
      bool include_psl,
      const std::vector<password_manager::PasswordFormDigest>& forms) override;
  void GetGroupedMatchingLoginsAsync(
      const password_manager::PasswordFormDigest& form_digest,
      password_manager::BackendLoginsOrErrorReply callback) override;
  void AddLoginAsync(
      password_manager::StoredCredential cred,
      password_manager::PasswordChangesOrErrorReply callback) override;
  void UpdateLoginAsync(
      password_manager::StoredCredential cred,
      password_manager::PasswordChangesOrErrorReply callback) override;
  void RemoveLoginAsync(
      const base::Location& location,
      password_manager::StoredCredential cred,
      password_manager::PasswordChangesOrErrorReply callback) override;
  void RemoveLoginsCreatedBetweenAsync(
      const base::Location& location,
      base::Time delete_begin,
      base::Time delete_end,
      password_manager::PasswordChangesOrErrorReply callback) override;
  void DisableAutoSignInForOriginsAsync(
      const base::RepeatingCallback<bool(const GURL&)>& origin_filter,
      base::OnceClosure completion) override;
  password_manager::SmartBubbleStatsStore* GetSmartBubbleStatsStore() override;
  std::unique_ptr<syncer::DataTypeControllerDelegate>
  CreateSyncControllerDelegate() override;
  void OnSyncServiceInitialized(syncer::SyncService* sync_service) override;
  base::WeakPtr<PasswordStoreBackend> AsWeakPtr() override;

 private:
  using StoredCredential = password_manager::StoredCredential;
  using ChangesReply = password_manager::PasswordChangesOrErrorReply;

  // One login of this profile.
  struct Item {
    Item();
    Item(Item&&);
    Item& operator=(Item&&);
    ~Item();

    shard::pb::Login login;  // As Shard last returned it (secrets too).
    StoredCredential cred;       // What the password manager sees.
  };

  // A write, run one at a time so that every CAS base is current.
  struct Op {
    enum class Kind { kPut, kDelete };
    Op();
    Op(Op&&);
    Op& operator=(Op&&);
    ~Op();

    Kind kind = Kind::kPut;
    // Shard id once known; "" finds the login by its key when the op
    // runs (none: a new login).
    std::string id;
    StoredCredential cred;  // kPut: the new content; kDelete: the old one.
    // UpdateLogin: a login that is not there is not created.
    bool update = false;
    bool retried = false;  // A CAS conflict was retried once already.
    // kPut over an existing login by AddLogin: reported as REMOVE + ADD, as
    // the login DB does.
    bool replace = false;
    base::OnceCallback<void(
        std::optional<password_manager::PasswordStoreChangeList>)>
        done;  // nullopt: the write failed.
  };

  bool usable() const { return client_ != nullptr; }

  // Reads.
  void Read(base::OnceCallback<std::vector<StoredCredential>()> compute,
            password_manager::BackendLoginsOrErrorReply callback);
  std::vector<StoredCredential> AllLogins(bool autofillable_only) const;
  std::vector<StoredCredential> MatchingLogins(
      const password_manager::PasswordFormDigest& form,
      bool include_psl,
      bool set_match_type) const;

  // Full load (ListLogins with data, every page).
  void StartLoad();
  void OnLoadPage(int generation,
                  std::map<std::string, shard::pb::Login> collected,
                  std::string cursor,
                  shard::Status status,
                  shard::pb::ListLoginsReply reply);
  void FinishLoad(std::map<std::string, shard::pb::Login> logins,
                  std::string cursor);
  void RetryLoadLater();

  // Incremental refresh (ListChanges, then GetLogins of what changed).
  void StartRefresh();
  void OnChanges(int generation,
                 shard::Status status,
                 shard::pb::ListChangesReply reply);
  void OnChangedLogins(int generation,
                       std::vector<std::string> deleted,
                       std::string cursor,
                       shard::Status status,
                       shard::pb::ListLoginsReply reply);
  void EndRefresh();

  // State changes; each returns what the password manager has to hear.
  password_manager::PasswordStoreChangeList SetLogin(
      const shard::pb::Login& login,
      StoredCredential cred);
  password_manager::PasswordStoreChangeList DropLogin(const std::string& id);
  void ClearLogins();
  bool Owns(const shard::pb::Login& login) const;
  StoredCredential CredentialFor(const shard::pb::Login& login,
                                 const std::string& id) const;
  password_manager::FormPrimaryKey PrimaryKeyFor(const std::string& id);
  const Item* FindByKey(const StoredCredential& cred) const;
  void NotifyChanges(password_manager::PasswordStoreChangeList changes);
  void NotifyReload();

  // Writes.
  void WriteThenReply(Op op, ChangesReply callback);
  void Enqueue(Op op);
  void RunNextOp();
  void OnPutDone(int generation,
                 shard::Status status,
                 shard::pb::Login login);
  void OnDeleteDone(int generation,
                    shard::Status status,
                    shard::pb::DeleteLoginReply reply);
  void OnConflictReread(int generation,
                        shard::Status status,
                        shard::pb::ListLoginsReply reply);
  void FinishOp(
      std::optional<password_manager::PasswordStoreChangeList> result);
  void FailAllOps();

  SEQUENCE_CHECKER(sequence_checker_);

  const base::FilePath profile_dir_;
  RemoteChangesReceived remote_changes_;
  bool shut_down_ = false;

  raw_ptr<shard::ConnectorClient> client_ = nullptr;
  std::string slot_;
  bool primary_ = false;
  // Bumped on every new connection: replies of an older one are dropped.
  int generation_ = 0;

  bool loaded_ = false;
  bool loading_ = false;
  bool refreshing_ = false;
  bool refresh_wanted_ = false;
  std::string cursor_;
  std::vector<base::OnceClosure> waiting_reads_;
  base::OneShotTimer retry_timer_;

  std::map<std::string, Item> items_;  // By Shard id.
  // Logins of other profiles, id -> revision: a change that keeps the
  // revision is not fetched.
  std::map<std::string, std::string> foreign_;
  std::map<password_sync::LoginKey, std::string> by_key_;
  // Stable primary keys for the password manager, by Shard id.
  std::map<std::string, int> primary_keys_;
  int next_primary_key_ = 1;

  base::circular_deque<Op> ops_;
  bool op_running_ = false;

  base::WeakPtrFactory<ShardPasswordBackend> weak_ptr_factory_{this};
};

}  // namespace enclave

#endif  // CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_PASSWORD_BACKEND_H_
