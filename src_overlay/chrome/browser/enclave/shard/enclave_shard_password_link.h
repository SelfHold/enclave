// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_PASSWORD_LINK_H_
#define CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_PASSWORD_LINK_H_

#include "base/files/file_path.h"

namespace enclave {

namespace shard {
class ShardController;
}

class ShardPasswordBackend;

// Joins a profile's ShardController (owned by EnclaveShardService) and
// its ShardPasswordBackend (owned by the profile's PasswordStore). Neither
// keyed service can depend on the other: SyncService already depends on the
// password store, and EnclaveShardService on SyncService. So each side
// checks in here under the profile directory, in any order, and checks out
// (null) on shutdown. While both are here, the backend follows the
// controller: connection, slot, primary flag and LOGIN changes. UI thread.
class ShardPasswordLink {
 public:
  ShardPasswordLink() = delete;

  static void SetController(const base::FilePath& profile_dir,
                            shard::ShardController* controller);
  static void SetBackend(const base::FilePath& profile_dir,
                         ShardPasswordBackend* backend);
};

}  // namespace enclave

#endif  // CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_PASSWORD_LINK_H_
