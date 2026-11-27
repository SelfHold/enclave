// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_service_factory.h"

#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_service.h"
#include "build/build_config.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/sync_service_factory.h"

namespace bezel {

// static
BezelCloudSyncService* BezelCloudSyncServiceFactory::GetForProfile(
    Profile* profile) {
  return static_cast<BezelCloudSyncService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
BezelCloudSyncServiceFactory* BezelCloudSyncServiceFactory::GetInstance() {
  static base::NoDestructor<BezelCloudSyncServiceFactory> instance;
  return instance.get();
}

BezelCloudSyncServiceFactory::BezelCloudSyncServiceFactory()
    : ProfileKeyedServiceFactory(
          "BezelCloudSyncService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {
  DependsOn(SyncServiceFactory::GetInstance());
#if !BUILDFLAG(IS_ANDROID)
  // The password mirror writes into the profile's login DB. On Android the
  // store depends on CloudSync instead (CloudSyncPasswordLink), the other
  // way round, and SyncService already depends on the store.
  DependsOn(ProfilePasswordStoreFactory::GetInstance());
#endif
}

BezelCloudSyncServiceFactory::~BezelCloudSyncServiceFactory() = default;

std::unique_ptr<KeyedService>
BezelCloudSyncServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<BezelCloudSyncService>(
      Profile::FromBrowserContext(context));
}

bool BezelCloudSyncServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  // Replication and the password mirror run whether or not a page asks.
  return true;
}

bool BezelCloudSyncServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace bezel
