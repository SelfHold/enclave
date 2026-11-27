// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/ui/webui/enclave_shard/enclave_shard_handler.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/enclave/shard/enclave_shard_service.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_ui.h"

namespace enclave {

namespace {

constexpr char kStateEvent[] = "shard-state";
constexpr char kRemoveDoneEvent[] = "shard-remove-done";

}  // namespace

EnclaveShardHandler::EnclaveShardHandler(Profile* profile)
    : profile_(profile) {}

EnclaveShardHandler::~EnclaveShardHandler() = default;

void EnclaveShardHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getState", base::BindRepeating(&EnclaveShardHandler::HandleGetState,
                                      base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "enable", base::BindRepeating(&EnclaveShardHandler::HandleEnable,
                                    base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "cancelPairing",
      base::BindRepeating(&EnclaveShardHandler::HandleCancelPairing,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "disable", base::BindRepeating(&EnclaveShardHandler::HandleDisable,
                                     base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "removeData",
      base::BindRepeating(&EnclaveShardHandler::HandleRemoveData,
                          base::Unretained(this)));
}

void EnclaveShardHandler::OnJavascriptAllowed() {
  shard::ShardController* sync = controller();
  if (sync && !observation_.IsObserving()) {
    observation_.Observe(sync);
  }
}

void EnclaveShardHandler::OnJavascriptDisallowed() {
  observation_.Reset();
}

void EnclaveShardHandler::OnShardStateChanged() {
  SendState();
}

shard::ShardController* EnclaveShardHandler::controller() {
  EnclaveShardService* service = EnclaveShardService::Get(profile_);
  return service ? &service->controller() : nullptr;
}

void EnclaveShardHandler::HandleGetState(const base::ListValue& args) {
  AllowJavascript();
  SendState();
}

void EnclaveShardHandler::HandleEnable(const base::ListValue& args) {
  AllowJavascript();
  if (shard::ShardController* sync = controller()) {
    sync->EnableSync();
  }
  SendState();
}

void EnclaveShardHandler::HandleCancelPairing(const base::ListValue& args) {
  AllowJavascript();
  if (shard::ShardController* sync = controller()) {
    sync->CancelPairing();
  }
  SendState();
}

void EnclaveShardHandler::HandleDisable(const base::ListValue& args) {
  AllowJavascript();
  if (shard::ShardController* sync = controller()) {
    sync->DisableSync();
  }
  SendState();
}

void EnclaveShardHandler::HandleRemoveData(const base::ListValue& args) {
  AllowJavascript();
  shard::ShardController* sync = controller();
  if (!sync) {
    OnRemoved(shard::Status::FromWire(
        static_cast<int>(shard::Code::kFailedPrecondition),
        "Sync via Shard is not available in this profile."));
    return;
  }
  if (remove_in_flight_) {
    return;
  }
  remove_in_flight_ = true;
  sync->RemoveSyncedData(base::BindOnce(&EnclaveShardHandler::OnRemoved,
                                        weak_factory_.GetWeakPtr()));
}

void EnclaveShardHandler::OnRemoved(shard::Status status) {
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

void EnclaveShardHandler::SendState() {
  if (!IsJavascriptAllowed()) {
    return;
  }
  shard::ShardController* sync = controller();
  if (!sync) {
    base::DictValue state;
    state.Set("phase", "unavailable");
    FireWebUIListener(kStateEvent, state);
    return;
  }
  FireWebUIListener(kStateEvent, sync->StateAsDict());
}

}  // namespace enclave
