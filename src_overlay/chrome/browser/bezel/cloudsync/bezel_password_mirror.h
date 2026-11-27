// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_PASSWORD_MIRROR_H_
#define CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_PASSWORD_MIRROR_H_

#include <memory>

#include "components/bezel_cloudsync/cloudsync_controller.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace bezel {

// Keeps the profile's password store (the local login DB) and CloudSync's
// password store (CONNECTOR.md § 12) in step while sync via CloudSync is on
// and the "passwords" scope is granted. The local DB stays the working copy
// and the offline cache (§ 12.3). Owned by BezelCloudSyncService.
class BezelPasswordMirror : public KeyedService {
 public:
  BezelPasswordMirror(Profile* profile,
                      cloudsync::CloudSyncController* controller);
  BezelPasswordMirror(const BezelPasswordMirror&) = delete;
  BezelPasswordMirror& operator=(const BezelPasswordMirror&) = delete;
  ~BezelPasswordMirror() override;

  // KeyedService:
  void Shutdown() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bezel

#endif  // CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_PASSWORD_MIRROR_H_
