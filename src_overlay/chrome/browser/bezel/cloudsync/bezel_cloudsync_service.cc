// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_service.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/version_info/version_info.h"
#include "build/build_config.h"
#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_service_factory.h"
#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_password_link.h"
#include "chrome/browser/bezel/cloudsync/bezel_password_mirror.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "components/bezel_cloudsync/shared_pairing.h"
#include "chrome/browser/sync/sync_service_factory.h"
#include "components/sync/service/sync_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

#if BUILDFLAG(IS_ANDROID)
#include "chrome/browser/bezel/cloudsync/android/android_connector_transport.h"
#else
#include "components/bezel_cloudsync/connector_address.h"
#include "components/bezel_cloudsync/cross_thread_transport.h"
#include "components/bezel_cloudsync/grpc_transport.h"
#endif

namespace bezel {

namespace {

std::unique_ptr<cloudsync::ConnectorTransport> CreateTransport() {
#if BUILDFLAG(IS_ANDROID)
  return CreateAndroidConnectorTransport();
#else
  // Sockets need an IO message pump: the gRPC side runs on the IO thread.
  return std::make_unique<cloudsync::CrossThreadTransport>(
      content::GetIOThreadTaskRunner({}),
      base::BindOnce([]() -> std::unique_ptr<cloudsync::ConnectorTransport> {
        return std::make_unique<cloudsync::GrpcTransport>(
            cloudsync::GetConnectorJsonPath());
      }));
#endif
}

// One pairing per device, shared by every profile (CONNECTOR.md § 5).
cloudsync::SharedPairing* GetSharedPairing(const base::FilePath& user_data_dir) {
  static base::NoDestructor<std::unique_ptr<cloudsync::SharedPairing>> pairing(
      std::make_unique<cloudsync::SharedPairing>(
          user_data_dir.Append(FILE_PATH_LITERAL("Enclave Sync"))
              .Append(FILE_PATH_LITERAL("pairing.json"))));
  return pairing->get();
}

std::string Description() {
  return "Enclave " + std::string(version_info::GetVersionNumber()) + " on " +
         std::string(version_info::GetOSType());
}

}  // namespace

BezelCloudSyncService::BezelCloudSyncService(Profile* profile)
    : profile_(profile) {
  if (ProfileManager* manager = g_browser_process->profile_manager()) {
    attributes_observation_.Observe(&manager->GetProfileAttributesStorage());
  }
  controller_ = std::make_unique<cloudsync::CloudSyncController>(
      CreateTransport(), GetSharedPairing(profile->GetPath().DirName()),
      BackendFileFor(profile->GetPath()),
      profile->GetPath().BaseName().AsUTF8Unsafe(), ProfileName(),
      Description(),
      base::BindRepeating(&BezelCloudSyncService::RefreshTypes,
                          base::Unretained(this)));
#if BUILDFLAG(IS_ANDROID)
  // No login DB on Android: the password store's backend is CloudSync itself
  // (CloudSyncPasswordBackend), joined to this controller by the link.
  CloudSyncPasswordLink::SetController(profile->GetPath(), controller_.get());
#else
  password_mirror_ =
      std::make_unique<BezelPasswordMirror>(profile, controller_.get());
#endif
  controller_->Start();
}

BezelCloudSyncService::~BezelCloudSyncService() = default;

// static
BezelCloudSyncService* BezelCloudSyncService::Get(Profile* profile) {
  return BezelCloudSyncServiceFactory::GetForProfile(profile);
}

// static
base::FilePath BezelCloudSyncService::BackendFileFor(
    const base::FilePath& profile_dir) {
  return profile_dir.Append(FILE_PATH_LITERAL("Bezel Sync"))
      .Append(FILE_PATH_LITERAL("profile.pb"));
}

std::string BezelCloudSyncService::ProfileName() const {
  if (ProfileManager* manager = g_browser_process->profile_manager()) {
    if (ProfileAttributesEntry* entry =
            manager->GetProfileAttributesStorage()
                .GetProfileAttributesWithPath(profile_->GetPath())) {
      return base::UTF16ToUTF8(entry->GetLocalProfileName());
    }
  }
  // Android has one profile and no profile names.
  return profile_->GetPath().BaseName().AsUTF8Unsafe();
}

void BezelCloudSyncService::OnProfileAdded(const base::FilePath& profile_path) {
  if (profile_path == profile_->GetPath()) {
    controller_->SetProfileName(ProfileName());
  }
}

void BezelCloudSyncService::OnProfileNameChanged(
    const base::FilePath& profile_path,
    const std::u16string& old_profile_name) {
  if (profile_path == profile_->GetPath()) {
    controller_->SetProfileName(ProfileName());
  }
}

void BezelCloudSyncService::Shutdown() {
  attributes_observation_.Reset();
  if (password_mirror_) {
    password_mirror_->Shutdown();
  }
  password_mirror_.reset();
  CloudSyncPasswordLink::SetController(profile_->GetPath(), nullptr);
  controller_->Shutdown();
}

void BezelCloudSyncService::RefreshTypes(syncer::DataTypeSet types) {
  syncer::SyncService* sync = SyncServiceFactory::GetForProfile(profile_);
  if (sync) {
    sync->TriggerRefresh(syncer::SyncService::TriggerRefreshSource::kLocalSync,
                         types);
  }
}

}  // namespace bezel
