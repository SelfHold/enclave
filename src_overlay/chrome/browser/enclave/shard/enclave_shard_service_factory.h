// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_SERVICE_FACTORY_H_
#define CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_SERVICE_FACTORY_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace enclave {

class EnclaveShardService;

class EnclaveShardServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static EnclaveShardService* GetForProfile(Profile* profile);
  static EnclaveShardServiceFactory* GetInstance();

  EnclaveShardServiceFactory(const EnclaveShardServiceFactory&) = delete;
  EnclaveShardServiceFactory& operator=(const EnclaveShardServiceFactory&) =
      delete;

 private:
  friend base::NoDestructor<EnclaveShardServiceFactory>;

  EnclaveShardServiceFactory();
  ~EnclaveShardServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace enclave

#endif  // CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_SHARD_SERVICE_FACTORY_H_
