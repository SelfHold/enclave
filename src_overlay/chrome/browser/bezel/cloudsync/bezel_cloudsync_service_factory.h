// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_SERVICE_FACTORY_H_
#define CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_SERVICE_FACTORY_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace bezel {

class BezelCloudSyncService;

class BezelCloudSyncServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static BezelCloudSyncService* GetForProfile(Profile* profile);
  static BezelCloudSyncServiceFactory* GetInstance();

  BezelCloudSyncServiceFactory(const BezelCloudSyncServiceFactory&) = delete;
  BezelCloudSyncServiceFactory& operator=(const BezelCloudSyncServiceFactory&) =
      delete;

 private:
  friend base::NoDestructor<BezelCloudSyncServiceFactory>;

  BezelCloudSyncServiceFactory();
  ~BezelCloudSyncServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;
  bool ServiceIsNULLWhileTesting() const override;
};

}  // namespace bezel

#endif  // CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_SERVICE_FACTORY_H_
