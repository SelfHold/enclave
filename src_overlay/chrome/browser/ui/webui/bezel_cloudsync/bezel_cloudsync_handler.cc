// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/ui/webui/bezel_cloudsync/bezel_cloudsync_handler.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_service.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_ui.h"

namespace bezel {

namespace {

constexpr char kStateEvent[] = "cloudsync-state";
constexpr char kRemoveDoneEvent[] = "cloudsync-remove-done";

}  // namespace

BezelCloudSyncHandler::BezelCloudSyncHandler(Profile* profile)
    : profile_(profile) {}

BezelCloudSyncHandler::~BezelCloudSyncHandler() = default;

void BezelCloudSyncHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getState", base::BindRepeating(&BezelCloudSyncHandler::HandleGetState,
                                      base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "enable", base::BindRepeating(&BezelCloudSyncHandler::HandleEnable,
                                    base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelPairing",
      base::BindRepeating(&BezelCloudSyncHandler::HandleCancelPairing,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "disable", base::BindRepeating(&BezelCloudSyncHandler::HandleDisable,
                                     base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "removeData",
      base::BindRepeating(&BezelCloudSyncHandler::HandleRemoveData,
                          base::Unretained(this)));
}

void BezelCloudSyncHandler::OnJavascriptAllowed() {
  cloudsync::CloudSyncController* sync = controller();
  if (sync && !observation_.IsObserving()) {
    observation_.Observe(sync);
  }
}

void BezelCloudSyncHandler::OnJavascriptDisallowed() {
  observation_.Reset();
}

void BezelCloudSyncHandler::OnCloudSyncStateChanged() {
  SendState();
}

cloudsync::CloudSyncController* BezelCloudSyncHandler::controller() {
  BezelCloudSyncService* service = BezelCloudSyncService::Get(profile_);
  return service ? &service->controller() : nullptr;
}

void BezelCloudSyncHandler::HandleGetState(const base::ListValue& args) {
  AllowJavascript();
  SendState();
}

void BezelCloudSyncHandler::HandleEnable(const base::ListValue& args) {
  AllowJavascript();
  if (cloudsync::CloudSyncController* sync = controller()) {
    sync->EnableSync();
  }
  SendState();
}

void BezelCloudSyncHandler::HandleCancelPairing(const base::ListValue& args) {
  AllowJavascript();
  if (cloudsync::CloudSyncController* sync = controller()) {
    sync->CancelPairing();
  }
  SendState();
}

void BezelCloudSyncHandler::HandleDisable(const base::ListValue& args) {
  AllowJavascript();
  if (cloudsync::CloudSyncController* sync = controller()) {
    sync->DisableSync();
  }
  SendState();
}

void BezelCloudSyncHandler::HandleRemoveData(const base::ListValue& args) {
  AllowJavascript();
  cloudsync::CloudSyncController* sync = controller();
  if (!sync) {
    OnRemoved(cloudsync::Status::FromWire(
        static_cast<int>(cloudsync::Code::kFailedPrecondition),
        "Sync via CloudSync is not available in this profile."));
    return;
  }
  if (remove_in_flight_) {
    return;
  }
  remove_in_flight_ = true;
  sync->RemoveSyncedData(base::BindOnce(&BezelCloudSyncHandler::OnRemoved,
                                        weak_factory_.GetWeakPtr()));
}

void BezelCloudSyncHandler::OnRemoved(cloudsync::Status status) {
  remove_in_flight_ = false;
  if (!IsJavascriptAllowed()) {
    return;
  }
  std::string message;
  if (!status.ok()) {
    message = status.message.empty() ? status.ToString() : status.message;
  }
  base::DictValue result;
  result.Set("ok", status.ok());
  result.Set("message", std::move(message));
  FireWebUIListener(kRemoveDoneEvent, result);
  SendState();
}

void BezelCloudSyncHandler::SendState() {
  if (!IsJavascriptAllowed()) {
    return;
  }
  cloudsync::CloudSyncController* sync = controller();
  if (!sync) {
    base::DictValue state;
    state.Set("phase", "unavailable");
    FireWebUIListener(kStateEvent, state);
    return;
  }
  FireWebUIListener(kStateEvent, sync->StateAsDict());
}

}  // namespace bezel
