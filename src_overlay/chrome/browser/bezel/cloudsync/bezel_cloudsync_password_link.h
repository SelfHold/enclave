// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_PASSWORD_LINK_H_
#define CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_PASSWORD_LINK_H_

#include "base/files/file_path.h"

namespace bezel {

namespace cloudsync {
class CloudSyncController;
}

class CloudSyncPasswordBackend;

// Joins a profile's CloudSyncController (owned by BezelCloudSyncService) and
// its CloudSyncPasswordBackend (owned by the profile's PasswordStore). Neither
// keyed service can depend on the other: SyncService already depends on the
// password store, and BezelCloudSyncService on SyncService. So each side
// checks in here under the profile directory, in any order, and checks out
// (null) on shutdown. While both are here, the backend follows the
// controller: connection, slot, primary flag and LOGIN changes. UI thread.
class CloudSyncPasswordLink {
 public:
  CloudSyncPasswordLink() = delete;

  static void SetController(const base::FilePath& profile_dir,
                            cloudsync::CloudSyncController* controller);
  static void SetBackend(const base::FilePath& profile_dir,
                         CloudSyncPasswordBackend* backend);
};

}  // namespace bezel

#endif  // CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_PASSWORD_LINK_H_
