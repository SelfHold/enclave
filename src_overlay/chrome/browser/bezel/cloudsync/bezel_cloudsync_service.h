// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_SERVICE_H_
#define CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_SERVICE_H_

#include <memory>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_attributes_storage_observer.h"
#include "components/bezel_cloudsync/cloudsync_controller.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/sync/base/data_type.h"

class Profile;

namespace bezel {

// "Sync via CloudSync" of one profile (docs/CLOUDSYNC.md). Created with the
// profile; owns the CloudSyncController and the password mirror.
class BezelCloudSyncService : public KeyedService,
                              public ProfileAttributesStorageObserver {
 public:
  explicit BezelCloudSyncService(Profile* profile);
  BezelCloudSyncService(const BezelCloudSyncService&) = delete;
  BezelCloudSyncService& operator=(const BezelCloudSyncService&) = delete;
  ~BezelCloudSyncService() override;

  // Null for profiles without sync (incognito, guest).
  static BezelCloudSyncService* Get(Profile* profile);

  // The LoopbackServer file of a profile; ChromeSyncClient uses the same
  // path (patch 0009).
  static base::FilePath BackendFileFor(const base::FilePath& profile_dir);

  cloudsync::CloudSyncController& controller() { return *controller_; }

  // KeyedService:
  void Shutdown() override;

  // ProfileAttributesStorageObserver:
  void OnProfileAdded(const base::FilePath& profile_path) override;
  void OnProfileNameChanged(const base::FilePath& profile_path,
                            const std::u16string& old_profile_name) override;

 private:
  std::string ProfileName() const;

  void RefreshTypes(syncer::DataTypeSet types);

  raw_ptr<Profile> profile_;
  base::ScopedObservation<ProfileAttributesStorage,
                          ProfileAttributesStorageObserver>
      attributes_observation_{this};
  std::unique_ptr<cloudsync::CloudSyncController> controller_;
  std::unique_ptr<KeyedService> password_mirror_;
};

}  // namespace bezel

#endif  // CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_SERVICE_H_
