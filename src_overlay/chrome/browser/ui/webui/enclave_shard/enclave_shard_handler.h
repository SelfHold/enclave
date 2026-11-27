// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "components/enclave_shard/shard_controller.h"
#include "components/enclave_shard/status.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace enclave {

// Messages of chrome://shard. From the page: "getState", "enable",
// "cancelPairing", "disable", "removeData". To the page (WebUI listener
// events): "shard-state" (the controller's StateAsDict(), or
// {phase: "unavailable"} for profiles without the service) and
// "shard-remove-done" ({ok, message}).
class EnclaveShardHandler
    : public content::WebUIMessageHandler,
      public shard::ShardController::Observer {
 public:
  explicit EnclaveShardHandler(Profile* profile);
  EnclaveShardHandler(const EnclaveShardHandler&) = delete;
  EnclaveShardHandler& operator=(const EnclaveShardHandler&) = delete;
  ~EnclaveShardHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // shard::ShardController::Observer:
  void OnShardStateChanged() override;

 private:
  // Null for profiles without "Sync via Shard" (incognito, guest).
  shard::ShardController* controller();

  void HandleGetState(const base::ListValue& args);
  void HandleEnable(const base::ListValue& args);
  void HandleCancelPairing(const base::ListValue& args);
  void HandleDisable(const base::ListValue& args);
  void HandleRemoveData(const base::ListValue& args);

  void OnRemoved(shard::Status status);
  void SendState();

  raw_ptr<Profile> profile_;
  bool remove_in_flight_ = false;
  base::ScopedObservation<shard::ShardController,
                          shard::ShardController::Observer>
      observation_{this};
  base::WeakPtrFactory<EnclaveShardHandler> weak_factory_{this};
};

}  // namespace enclave

#endif  // CHROME_BROWSER_UI_WEBUI_ENCLAVE_SHARD_ENCLAVE_SHARD_HANDLER_H_
