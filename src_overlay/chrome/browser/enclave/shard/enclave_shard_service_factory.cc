// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/enclave/shard/enclave_shard_service_factory.h"

#include "chrome/browser/enclave/shard/enclave_shard_service.h"
#include "build/build_config.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/sync/sync_service_factory.h"

namespace enclave {

// static
EnclaveShardService* EnclaveShardServiceFactory::GetForProfile(
    Profile* profile) {
  return static_cast<EnclaveShardService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
EnclaveShardServiceFactory* EnclaveShardServiceFactory::GetInstance() {
  static base::NoDestructor<EnclaveShardServiceFactory> instance;
  return instance.get();
}

EnclaveShardServiceFactory::EnclaveShardServiceFactory()
    : ProfileKeyedServiceFactory(
          "EnclaveShardService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .WithGuest(ProfileSelection::kNone)
              .WithSystem(ProfileSelection::kNone)
              .WithAshInternals(ProfileSelection::kNone)
              .Build()) {
  DependsOn(SyncServiceFactory::GetInstance());
#if !BUILDFLAG(IS_ANDROID)
  // The password mirror writes into the profile's login DB. On Android the
  // store depends on Shard instead (ShardPasswordLink), the other
  // way round, and SyncService already depends on the store.
  DependsOn(ProfilePasswordStoreFactory::GetInstance());
#endif
}

EnclaveShardServiceFactory::~EnclaveShardServiceFactory() = default;

std::unique_ptr<KeyedService>
EnclaveShardServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<EnclaveShardService>(
      Profile::FromBrowserContext(context));
}

bool EnclaveShardServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  // Replication and the password mirror run whether or not a page asks.
  return true;
}

bool EnclaveShardServiceFactory::ServiceIsNULLWhileTesting() const {
  return true;
}

}  // namespace enclave
