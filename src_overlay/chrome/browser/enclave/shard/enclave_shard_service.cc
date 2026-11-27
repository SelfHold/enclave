// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/enclave/shard/enclave_shard_service.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/version_info/version_info.h"
#include "build/build_config.h"
#include "chrome/browser/enclave/shard/enclave_shard_service_factory.h"
#include "chrome/browser/enclave/shard/enclave_shard_password_link.h"
#include "chrome/browser/enclave/shard/enclave_password_mirror.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "components/enclave_shard/shared_pairing.h"
#include "chrome/browser/sync/sync_service_factory.h"
#include "components/sync/service/sync_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

#if BUILDFLAG(IS_ANDROID)
#include "chrome/browser/enclave/shard/android/android_connector_transport.h"
#else
#include "components/enclave_shard/connector_address.h"
#include "components/enclave_shard/cross_thread_transport.h"
#include "components/enclave_shard/grpc_transport.h"
#endif

namespace enclave {

namespace {

std::unique_ptr<shard::ConnectorTransport> CreateTransport() {
#if BUILDFLAG(IS_ANDROID)
  return CreateAndroidConnectorTransport();
#else
  // Sockets need an IO message pump: the gRPC side runs on the IO thread.
  return std::make_unique<shard::CrossThreadTransport>(
      content::GetIOThreadTaskRunner({}),
      base::BindOnce([]() -> std::unique_ptr<shard::ConnectorTransport> {
        return std::make_unique<shard::GrpcTransport>(
            shard::GetConnectorJsonPath());
      }));
#endif
}

// One pairing per device, shared by every profile (CONNECTOR.md § 5).
shard::SharedPairing* GetSharedPairing(const base::FilePath& user_data_dir) {
  static base::NoDestructor<std::unique_ptr<shard::SharedPairing>> pairing(
      std::make_unique<shard::SharedPairing>(
          user_data_dir.Append(FILE_PATH_LITERAL("Enclave Sync"))
              .Append(FILE_PATH_LITERAL("pairing.json"))));
  return pairing->get();
}

std::string Description() {
  return "Enclave " + std::string(version_info::GetVersionNumber()) + " on " +
         std::string(version_info::GetOSType());
}

}  // namespace

EnclaveShardService::EnclaveShardService(Profile* profile)
    : profile_(profile) {
  if (ProfileManager* manager = g_browser_process->profile_manager()) {
    attributes_observation_.Observe(&manager->GetProfileAttributesStorage());
  }
  controller_ = std::make_unique<shard::ShardController>(
      CreateTransport(), GetSharedPairing(profile->GetPath().DirName()),
      BackendFileFor(profile->GetPath()),
      profile->GetPath().BaseName().AsUTF8Unsafe(), ProfileName(),
      Description(),
      base::BindRepeating(&EnclaveShardService::RefreshTypes,
                          base::Unretained(this)));
#if BUILDFLAG(IS_ANDROID)
  // No login DB on Android: the password store's backend is Shard itself
  // (ShardPasswordBackend), joined to this controller by the link.
  ShardPasswordLink::SetController(profile->GetPath(), controller_.get());
#else
  password_mirror_ =
      std::make_unique<EnclavePasswordMirror>(profile, controller_.get());
#endif
  controller_->Start();
}

EnclaveShardService::~EnclaveShardService() = default;

// static
EnclaveShardService* EnclaveShardService::Get(Profile* profile) {
  return EnclaveShardServiceFactory::GetForProfile(profile);
}

// static
base::FilePath EnclaveShardService::BackendFileFor(
    const base::FilePath& profile_dir) {
  return profile_dir.Append(FILE_PATH_LITERAL("Enclave Sync"))
      .Append(FILE_PATH_LITERAL("profile.pb"));
}

std::string EnclaveShardService::ProfileName() const {
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

void EnclaveShardService::OnProfileAdded(const base::FilePath& profile_path) {
  if (profile_path == profile_->GetPath()) {
    controller_->SetProfileName(ProfileName());
  }
}

void EnclaveShardService::OnProfileNameChanged(
    const base::FilePath& profile_path,
    const std::u16string& old_profile_name) {
  if (profile_path == profile_->GetPath()) {
    controller_->SetProfileName(ProfileName());
  }
}

void EnclaveShardService::Shutdown() {
  attributes_observation_.Reset();
  if (password_mirror_) {
    password_mirror_->Shutdown();
  }
  password_mirror_.reset();
  ShardPasswordLink::SetController(profile_->GetPath(), nullptr);
  controller_->Shutdown();
}

void EnclaveShardService::RefreshTypes(syncer::DataTypeSet types) {
  syncer::SyncService* sync = SyncServiceFactory::GetForProfile(profile_);
  if (sync) {
    sync->TriggerRefresh(syncer::SyncService::TriggerRefreshSource::kLocalSync,
                         types);
  }
}

}  // namespace enclave
