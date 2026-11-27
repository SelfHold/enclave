// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_ENCLAVE_SHARD_SHARED_PAIRING_H_
#define COMPONENTS_ENCLAVE_SHARD_SHARED_PAIRING_H_

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"

namespace enclave::shard {

// The browser's pairing with Shard on this device, shared by every
// profile: Shard knows one pairing per app and device (CONNECTOR.md § 5),
// so a second profile pairing on its own would revoke the first one's token.
// Kept in `<user data>/Enclave Sync/pairing.json` (mode 0600). UI thread.
class SharedPairing {
 public:
  struct Data {
    Data();
    Data(const Data&);
    Data& operator=(const Data&);
    ~Data();

    std::string token;  // Desktop bearer token; "" on Android.
    bool paired = false;
    std::vector<std::string> scopes;
    std::string device_id;  // Names this device in replicated entries.
    // Profiles (directory base names) with sync switched on: the pairing is
    // given up only when the last one switches off.
    std::set<std::string> enabled_profiles;
  };

  class Observer : public base::CheckedObserver {
   public:
    // Loaded, or another profile paired, unpaired or switched sync on/off.
    virtual void OnSharedPairingChanged() = 0;
  };

  explicit SharedPairing(const base::FilePath& file);
  SharedPairing(const SharedPairing&) = delete;
  SharedPairing& operator=(const SharedPairing&) = delete;
  ~SharedPairing();

  bool loaded() const { return loaded_; }
  const Data& data() const { return data_; }

  // Pairing approved (by any profile).
  void SetPaired(std::string token, std::vector<std::string> scopes);
  void SetScopes(std::vector<std::string> scopes);
  // Pairing gone (revoked in Shard, or Unpair). Every profile stops.
  void ClearPairing();
  void SetProfileEnabled(const std::string& profile, bool enabled);
  // Some other profile than `profile` has sync on.
  bool OthersEnabled(const std::string& profile) const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 private:
  void OnLoaded(std::optional<std::string> text);
  void SaveAndNotify();

  const base::FilePath file_;
  // Writes in order.
  scoped_refptr<base::SequencedTaskRunner> file_runner_;
  bool loaded_ = false;
  Data data_;
  base::ObserverList<Observer> observers_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<SharedPairing> weak_factory_{this};
};

}  // namespace enclave::shard

#endif  // COMPONENTS_ENCLAVE_SHARD_SHARED_PAIRING_H_
