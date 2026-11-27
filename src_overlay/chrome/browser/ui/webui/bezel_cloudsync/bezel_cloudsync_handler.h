// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/values.h"
#include "components/bezel_cloudsync/cloudsync_controller.h"
#include "components/bezel_cloudsync/status.h"
#include "content/public/browser/web_ui_message_handler.h"

class Profile;

namespace bezel {

// Messages of chrome://cloudsync. From the page: "getState", "enable",
// "cancelPairing", "disable", "removeData". To the page (WebUI listener
// events): "cloudsync-state" (the controller's StateAsDict(), or
// {phase: "unavailable"} for profiles without the service) and
// "cloudsync-remove-done" ({ok, message}).
class BezelCloudSyncHandler
    : public content::WebUIMessageHandler,
      public cloudsync::CloudSyncController::Observer {
 public:
  explicit BezelCloudSyncHandler(Profile* profile);
  BezelCloudSyncHandler(const BezelCloudSyncHandler&) = delete;
  BezelCloudSyncHandler& operator=(const BezelCloudSyncHandler&) = delete;
  ~BezelCloudSyncHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // cloudsync::CloudSyncController::Observer:
  void OnCloudSyncStateChanged() override;

 private:
  // Null for profiles without "Sync via CloudSync" (incognito, guest).
  cloudsync::CloudSyncController* controller();

  void HandleGetState(const base::ListValue& args);
  void HandleEnable(const base::ListValue& args);
  void HandleCancelPairing(const base::ListValue& args);
  void HandleDisable(const base::ListValue& args);
  void HandleRemoveData(const base::ListValue& args);

  void OnRemoved(cloudsync::Status status);
  void SendState();

  raw_ptr<Profile> profile_;
  bool remove_in_flight_ = false;
  base::ScopedObservation<cloudsync::CloudSyncController,
                          cloudsync::CloudSyncController::Observer>
      observation_{this};
  base::WeakPtrFactory<BezelCloudSyncHandler> weak_factory_{this};
};

}  // namespace bezel

#endif  // CHROME_BROWSER_UI_WEBUI_BEZEL_CLOUDSYNC_BEZEL_CLOUDSYNC_HANDLER_H_
