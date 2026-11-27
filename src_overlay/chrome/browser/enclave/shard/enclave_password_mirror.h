// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_PASSWORD_MIRROR_H_
#define CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_PASSWORD_MIRROR_H_

#include <memory>

#include "components/enclave_shard/shard_controller.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace enclave {

// Keeps the profile's password store (the local login DB) and Shard's
// password store (CONNECTOR.md § 12) in step while sync via Shard is on
// and the "passwords" scope is granted. The local DB stays the working copy
// and the offline cache (§ 12.3). Owned by EnclaveShardService.
class EnclavePasswordMirror : public KeyedService {
 public:
  EnclavePasswordMirror(Profile* profile,
                      shard::ShardController* controller);
  EnclavePasswordMirror(const EnclavePasswordMirror&) = delete;
  EnclavePasswordMirror& operator=(const EnclavePasswordMirror&) = delete;
  ~EnclavePasswordMirror() override;

  // KeyedService:
  void Shutdown() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace enclave

#endif  // CHROME_BROWSER_ENCLAVE_SHARD_ENCLAVE_PASSWORD_MIRROR_H_
