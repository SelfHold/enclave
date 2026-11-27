// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_password_link.h"

#include <map>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_password_backend.h"
#include "components/bezel_cloudsync/cloudsync_controller.h"

namespace bezel {

namespace {

// One profile's pair.
class Link : public cloudsync::CloudSyncController::Observer {
 public:
  Link() = default;
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;
  ~Link() override = default;

  bool empty() const { return !controller_ && !backend_; }

  void SetController(cloudsync::CloudSyncController* controller) {
    if (controller == controller_) {
      return;
    }
    observation_.Reset();
    controller_ = controller;
    if (controller_) {
      observation_.Observe(controller_);
    }
    Push();
  }

  void SetBackend(CloudSyncPasswordBackend* backend) {
    backend_ = backend;
    Push();
  }

  // CloudSyncController::Observer:
  void OnCloudSyncStateChanged() override { Push(); }
  void OnCloudSyncLoginsChanged() override {
    if (backend_) {
      backend_->OnRemoteLoginsChanged();
    }
  }

 private:
  void Push() {
    if (!backend_) {
      return;
    }
    if (!controller_ || !controller_->CanUsePasswords()) {
      backend_->SetConnection(nullptr, std::string(), false);
      return;
    }
    backend_->SetConnection(controller_->client(), controller_->profile_slot(),
                            controller_->is_primary_slot());
  }

  raw_ptr<cloudsync::CloudSyncController> controller_ = nullptr;
  raw_ptr<CloudSyncPasswordBackend> backend_ = nullptr;
  base::ScopedObservation<cloudsync::CloudSyncController,
                          cloudsync::CloudSyncController::Observer>
      observation_{this};
};

std::map<base::FilePath, std::unique_ptr<Link>>& Links() {
  static base::NoDestructor<std::map<base::FilePath, std::unique_ptr<Link>>>
      links;
  return *links;
}

Link* LinkFor(const base::FilePath& profile_dir) {
  std::unique_ptr<Link>& link = Links()[profile_dir];
  if (!link) {
    link = std::make_unique<Link>();
  }
  return link.get();
}

void DropIfEmpty(const base::FilePath& profile_dir) {
  auto it = Links().find(profile_dir);
  if (it != Links().end() && it->second->empty()) {
    Links().erase(it);
  }
}

}  // namespace

// static
void CloudSyncPasswordLink::SetController(
    const base::FilePath& profile_dir,
    cloudsync::CloudSyncController* controller) {
  LinkFor(profile_dir)->SetController(controller);
  DropIfEmpty(profile_dir);
}

// static
void CloudSyncPasswordLink::SetBackend(const base::FilePath& profile_dir,
                                       CloudSyncPasswordBackend* backend) {
  LinkFor(profile_dir)->SetBackend(backend);
  DropIfEmpty(profile_dir);
}

}  // namespace bezel
