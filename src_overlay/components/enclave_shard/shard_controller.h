// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_ENCLAVE_SHARD_SHARD_CONTROLLER_H_
#define COMPONENTS_ENCLAVE_SHARD_SHARD_CONTROLLER_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "components/enclave_shard/connector_client.h"
#include "components/enclave_shard/proto/enclave_records.pb.h"
#include "components/enclave_shard/replicator.h"
#include "components/enclave_shard/shared_pairing.h"

namespace enclave::shard {

// "Sync via Shard" for one browser profile: watches the vault state,
// finds the profile's slot in the "profiles" record (by name, so that
// profiles with the same name on different devices share their data) and
// runs the Replicator under "p/<slot>/" while sync is on.
//
// The pairing itself is the device's (SharedPairing): pairing in one profile
// pairs them all, and the pairing is given up when the last profile switches
// sync off. Per-profile state (switch, vault id, slot) is kept in
// `<backend dir>/shard_state.json`. UI thread.
class ShardController : public ConnectorClient::WatchObserver,
                            public SharedPairing::Observer {
 public:
  enum class Phase {
    kStarting,
    kNotRunning,    // Shard is not installed or not running.
    kIncompatible,  // Shard speaks another API version.
    kNoVault,
    kLocked,
    kOffline,
    kReady,
  };

  struct State {
    State();
    State(const State&);
    ~State();

    Phase phase = Phase::kStarting;
    bool enabled = false;  // The user switched sync on in this profile.
    bool paired = false;   // This device is paired (any profile).
    bool passwords_scope = false;
    bool pairing = false;      // Waiting for the user in Shard.
    std::string pairing_code;  // "123 456" while pairing.
    std::string message;       // Last notice for the user ("" = none).
    std::string vault_id;
    std::string device_name;
    std::string core_version;
    std::string profile_name;  // This profile's local name.
    std::string slot_id;       // "" until known.
    std::string slot_name;
    bool primary_slot = false;
    // Every profile slot of the vault, by name, for the page.
    std::vector<std::string> slot_names;
    Replicator::Stats replication;
  };

  class Observer : public base::CheckedObserver {
   public:
    virtual void OnShardStateChanged() {}
    // A login of Shard's password store changed (Watch LOGIN).
    virtual void OnShardLoginsChanged() {}
  };

  // `profile_key` names the profile in SharedPairing (its directory's base
  // name); `profile_name` is the name shown to the user and matched against
  // slots. `description` goes into pairing requests.
  ShardController(std::unique_ptr<ConnectorTransport> transport,
                      SharedPairing* shared_pairing,
                      const base::FilePath& backend_file,
                      std::string profile_key,
                      std::string profile_name,
                      std::string description,
                      Replicator::RefreshCallback refresh);
  ShardController(const ShardController&) = delete;
  ShardController& operator=(const ShardController&) = delete;
  ~ShardController() override;

  void Start();
  void Shutdown();

  // Pairs the device if needed (the user confirms the code in Shard),
  // finds or makes this profile's slot, then turns replication on. Also
  // adopts a new vault after "another vault".
  void EnableSync();
  void CancelPairing();
  // Keeps local data and the vault's records. Unpairs the device when no
  // other profile syncs.
  void DisableSync();
  // Deletes this profile's records from the vault, then switches sync off.
  void RemoveSyncedData(base::OnceCallback<void(Status)> done);
  // The profile was renamed: renames its slot, or joins another device's
  // profile of that name.
  void SetProfileName(std::string name);

  const State& state() const { return state_; }
  base::DictValue StateAsDict() const;

  // For the password mirror. Valid until Shutdown.
  ConnectorClient* client() { return client_.get(); }
  // Sync is on, paired, and the "passwords" scope was granted.
  bool CanUsePasswords() const;
  // This profile's slot ("" until known) and whether it owns logins that
  // carry no profile tag.
  const std::string& profile_slot() const { return state_.slot_id; }
  bool is_primary_slot() const { return state_.primary_slot; }

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // ConnectorClient::WatchObserver:
  void OnChange(const pb::Change& change) override;
  void OnWatchEnd(Status status) override;
  // SharedPairing::Observer:
  void OnSharedPairingChanged() override;

  // HKDF-SHA256(AppKey, "enclave-keystore-v1/" + slot), 16 bytes like the
  // loopback server's own keys. Public for tests.
  static std::vector<uint8_t> KeystoreKeyFromAppKey(const std::string& app_key,
                                                    const std::string& slot);
  // "  Work " and "work" are one profile.
  static std::string NormalizeProfileName(const std::string& name);
  // The slot a profile named `name` uses, given the slot it used before
  // (`current`, may be ""), the name it had then, and the vault's slots.
  // Returns the slot id, or "" if a new slot must be made. Sets `rename` when
  // the current slot should take the new name. Public for tests.
  static std::string ChooseSlot(const records::ProfileSlots& slots,
                                const std::string& current,
                                const std::string& synced_name,
                                const std::string& name,
                                bool* rename);
  // Union of several versions of "profiles": by id; the newest name wins.
  static records::ProfileSlots MergeSlots(
      const std::vector<records::ProfileSlots>& versions);

 private:
  struct Saved {
    bool enabled = false;
    std::string vault_id;
    std::string key_id;
    std::string slot_id;
    std::string slot_name;  // The name the slot had when last synced.
  };

  void OnSavedLoaded(std::optional<std::string> text);
  void Save();
  void Hello();
  void OnHello(int generation, Status status, pb::HelloReply reply);
  void RetryHelloIn(base::TimeDelta delay);
  void MaybeStartReplication();
  void ResolveSlot();
  void OnSlotsRead(Status status, pb::Item item);
  void OnSlotsWritten(std::string slot_id, Status status, pb::Item item);
  void UseSlot(const std::string& slot_id, const records::ProfileSlots& slots);
  void OnAppKey(Status status, pb::AppKeyReply reply);
  void RequestPairing();
  void OnPairingTicket(Status status, pb::PairingTicket ticket);
  void AwaitPairing();
  void OnPairingResult(Status status, pb::PairingResult result);
  void HandleUnpaired(std::string message);
  void StopReplication();
  void Notify();

  raw_ptr<SharedPairing> shared_;
  const base::FilePath backend_file_;
  const base::FilePath state_file_;
  const std::string profile_key_;
  const std::string description_;
  std::unique_ptr<ConnectorClient> client_;
  std::unique_ptr<Replicator> replicator_;
  std::string replicator_slot_;
  Replicator::RefreshCallback refresh_;
  std::unique_ptr<ConnectorTransport::WatchHandle> watch_;
  scoped_refptr<base::SequencedTaskRunner> file_runner_;

  Saved saved_;
  bool loaded_ = false;
  bool shut_down_ = false;
  State state_;
  std::string ticket_;
  bool hello_in_flight_ = false;
  // Bumped whenever the token or paired flag changes; a Hello reply from an
  // older generation is ignored.
  int pairing_generation_ = 0;
  std::string applied_token_;
  bool applied_paired_ = false;
  bool slot_in_flight_ = false;
  bool app_key_in_flight_ = false;
  std::string slots_revision_;
  base::TimeDelta backoff_;
  base::OneShotTimer retry_timer_;

  base::ObserverList<Observer> observers_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ShardController> weak_factory_{this};
};

}  // namespace enclave::shard

#endif  // COMPONENTS_ENCLAVE_SHARD_SHARD_CONTROLLER_H_
