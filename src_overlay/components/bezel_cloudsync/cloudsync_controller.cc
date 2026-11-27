// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/bezel_cloudsync/cloudsync_controller.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/i18n/case_conversion.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/bezel_cloudsync/connector_address.h"
#include "crypto/hash.h"
#include "crypto/kdf.h"

namespace bezel::cloudsync {

namespace {

constexpr base::FilePath::CharType kStateFileName[] =
    FILE_PATH_LITERAL("cloudsync_state.json");
constexpr char kPasswordsScope[] = "passwords";
constexpr char kProfilesKey[] = "profiles";
constexpr char kSlotPrefix[] = "p/";
constexpr char kKeystoreInfo[] = "bezel-keystore-v1/";
constexpr size_t kKeystoreKeyLength = 16;  // As LoopbackServer's own keys.

constexpr base::TimeDelta kNotRunningRetry = base::Seconds(30);
constexpr base::TimeDelta kIncompatibleRetry = base::Minutes(5);
constexpr base::TimeDelta kMinBackoff = base::Seconds(5);
constexpr base::TimeDelta kMaxBackoff = base::Seconds(60);

std::optional<std::string> ReadFile(base::FilePath path) {
  std::string text;
  if (!base::ReadFileToStringWithMaxSize(path, &text, 64 * 1024)) {
    return std::nullopt;
  }
  return text;
}

void WriteFile(base::FilePath path, std::string text) {
  if (!base::CreateDirectory(path.DirName())) {
    return;
  }
  base::ImportantFileWriter::WriteFileAtomically(path, text);
}

CloudSyncController::Phase PhaseOf(pb::VaultState state) {
  switch (state) {
    case pb::NO_VAULT:
      return CloudSyncController::Phase::kNoVault;
    case pb::LOCKED:
      return CloudSyncController::Phase::kLocked;
    case pb::OFFLINE:
      return CloudSyncController::Phase::kOffline;
    case pb::READY:
      return CloudSyncController::Phase::kReady;
    default:
      // Unknown values are treated as unspecified (§ 11): not usable.
      return CloudSyncController::Phase::kNoVault;
  }
}

const char* PhaseName(CloudSyncController::Phase phase) {
  switch (phase) {
    case CloudSyncController::Phase::kStarting:
      return "starting";
    case CloudSyncController::Phase::kNotRunning:
      return "not_running";
    case CloudSyncController::Phase::kIncompatible:
      return "incompatible";
    case CloudSyncController::Phase::kNoVault:
      return "no_vault";
    case CloudSyncController::Phase::kLocked:
      return "locked";
    case CloudSyncController::Phase::kOffline:
      return "offline";
    case CloudSyncController::Phase::kReady:
      return "ready";
  }
}

const records::ProfileSlot* FindSlot(const records::ProfileSlots& slots,
                                     const std::string& id) {
  for (const records::ProfileSlot& slot : slots.slots()) {
    if (slot.id() == id) {
      return &slot;
    }
  }
  return nullptr;
}

bool OlderSlot(const records::ProfileSlot& a, const records::ProfileSlot& b) {
  return a.created_ms() != b.created_ms() ? a.created_ms() < b.created_ms()
                                          : a.id() < b.id();
}

// The oldest slot with this (normalized) name, or nullptr.
const records::ProfileSlot* SlotNamed(const records::ProfileSlots& slots,
                                      const std::string& normalized,
                                      const std::string& except_id) {
  const records::ProfileSlot* best = nullptr;
  for (const records::ProfileSlot& slot : slots.slots()) {
    if (slot.id() != except_id &&
        CloudSyncController::NormalizeProfileName(slot.name()) == normalized &&
        (!best || OlderSlot(slot, *best))) {
      best = &slot;
    }
  }
  return best;
}

std::string NewSlotId() {
  uint32_t value = base::RandUint64() & 0xffffffff;
  std::string hex = base::HexEncode(base::byte_span_from_ref(value));
  return base::ToLowerASCII(hex);
}

}  // namespace

CloudSyncController::State::State() = default;
CloudSyncController::State::State(const State&) = default;
CloudSyncController::State::~State() = default;

CloudSyncController::CloudSyncController(
    std::unique_ptr<ConnectorTransport> transport,
    SharedPairing* shared_pairing,
    const base::FilePath& backend_file,
    std::string profile_key,
    std::string profile_name,
    std::string description,
    Replicator::RefreshCallback refresh)
    : shared_(shared_pairing),
      backend_file_(backend_file),
      state_file_(backend_file.DirName().Append(kStateFileName)),
      profile_key_(std::move(profile_key)),
      description_(std::move(description)),
      client_(std::make_unique<ConnectorClient>(std::move(transport))),
      refresh_(std::move(refresh)),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  state_.profile_name = std::move(profile_name);
}

CloudSyncController::~CloudSyncController() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

// static
std::vector<uint8_t> CloudSyncController::KeystoreKeyFromAppKey(
    const std::string& app_key,
    const std::string& slot) {
  const std::string info = kKeystoreInfo + slot;
  std::vector<uint8_t> key(kKeystoreKeyLength);
  crypto::kdf::Hkdf(crypto::hash::kSha256, base::as_byte_span(app_key),
                    /*salt=*/{}, base::as_byte_span(info), key);
  return key;
}

// static
std::string CloudSyncController::NormalizeProfileName(const std::string& name) {
  std::u16string text = base::UTF8ToUTF16(name);
  base::TrimWhitespace(text, base::TRIM_ALL, &text);
  return base::UTF16ToUTF8(base::i18n::FoldCase(text));
}

// static
std::string CloudSyncController::ChooseSlot(
    const records::ProfileSlots& slots,
    const std::string& current,
    const std::string& synced_name,
    const std::string& name,
    bool* rename) {
  *rename = false;
  const std::string wanted = NormalizeProfileName(name);
  const records::ProfileSlot* mine =
      current.empty() ? nullptr : FindSlot(slots, current);
  if (mine) {
    if (synced_name.empty() || NormalizeProfileName(synced_name) == wanted) {
      return current;  // Not renamed here; a rename elsewhere is kept.
    }
    // Renamed here: join a profile of that name, or rename the slot.
    if (const records::ProfileSlot* other = SlotNamed(slots, wanted, current)) {
      return other->id();
    }
    *rename = NormalizeProfileName(mine->name()) != wanted;
    return current;
  }
  if (const records::ProfileSlot* same = SlotNamed(slots, wanted, "")) {
    return same->id();
  }
  return std::string();
}

// static
records::ProfileSlots CloudSyncController::MergeSlots(
    const std::vector<records::ProfileSlots>& versions) {
  std::map<std::string, records::ProfileSlot> by_id;
  for (const records::ProfileSlots& version : versions) {
    for (const records::ProfileSlot& slot : version.slots()) {
      auto [it, inserted] = by_id.try_emplace(slot.id(), slot);
      if (inserted) {
        continue;
      }
      records::ProfileSlot& merged = it->second;
      if (slot.renamed_ms() > merged.renamed_ms() ||
          (slot.renamed_ms() == merged.renamed_ms() &&
           slot.name() > merged.name())) {
        merged.set_name(slot.name());
        merged.set_renamed_ms(slot.renamed_ms());
      }
      merged.set_created_ms(std::min(merged.created_ms(), slot.created_ms()));
    }
  }
  records::ProfileSlots result;
  result.set_format(1);
  for (auto& [id, slot] : by_id) {
    *result.add_slots() = std::move(slot);
  }
  return result;
}

// --- start / stop --------------------------------------------------------------

void CloudSyncController::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  shared_->AddObserver(this);
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadFile, state_file_),
      base::BindOnce(&CloudSyncController::OnSavedLoaded,
                     weak_factory_.GetWeakPtr()));
}

void CloudSyncController::OnSavedLoaded(std::optional<std::string> text) {
  if (text) {
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(*text, base::JSON_PARSE_RFC);
    if (dict) {
      saved_.enabled = dict->FindBool("enabled").value_or(false);
      if (const std::string* s = dict->FindString("vault_id")) {
        saved_.vault_id = *s;
      }
      if (const std::string* s = dict->FindString("key_id")) {
        saved_.key_id = *s;
      }
      if (const std::string* s = dict->FindString("slot_id")) {
        saved_.slot_id = *s;
      }
      if (const std::string* s = dict->FindString("slot_name")) {
        saved_.slot_name = *s;
      }
    }
  }
  loaded_ = true;
  state_.enabled = saved_.enabled;
  if (shared_->loaded()) {
    applied_token_ = shared_->data().token;
    applied_paired_ = shared_->data().paired;
    ++pairing_generation_;
    client_->SetToken(applied_token_);
    shared_->SetProfileEnabled(profile_key_, saved_.enabled);
    Hello();
  }
  // Otherwise OnSharedPairingChanged starts us once it is loaded.
}

void CloudSyncController::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  shut_down_ = true;
  shared_->RemoveObserver(this);
  retry_timer_.Stop();
  watch_.reset();
  if (replicator_) {
    replicator_->FlushNow();
    replicator_->Stop();
  }
  replicator_.reset();
  weak_factory_.InvalidateWeakPtrs();
}

void CloudSyncController::Save() {
  base::DictValue dict;
  dict.Set("enabled", saved_.enabled);
  dict.Set("vault_id", saved_.vault_id);
  dict.Set("key_id", saved_.key_id);
  dict.Set("slot_id", saved_.slot_id);
  dict.Set("slot_name", saved_.slot_name);
  std::string text;
  base::JSONWriter::Write(dict, &text);
  file_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&WriteFile, state_file_, std::move(text)));
}

// --- shared pairing ------------------------------------------------------------

void CloudSyncController::OnSharedPairingChanged() {
  if (shut_down_ || !loaded_) {
    return;
  }
  const SharedPairing::Data& data = shared_->data();
  if (data.token != applied_token_ || data.paired != applied_paired_) {
    applied_token_ = data.token;
    applied_paired_ = data.paired;
    ++pairing_generation_;
    client_->SetToken(data.token);
  }
  if (!data.paired && state_.paired) {
    // Another profile unpaired the device (or saw it revoked).
    StopReplication();
    watch_.reset();
    state_.paired = false;
    state_.passwords_scope = false;
    if (saved_.enabled) {
      saved_.enabled = false;
      state_.enabled = false;
      Save();
    }
  }
  if (saved_.enabled && !data.enabled_profiles.contains(profile_key_) &&
      data.paired) {
    shared_->SetProfileEnabled(profile_key_, true);  // Notifies again.
    return;
  }
  Hello();
  Notify();
}

// --- Hello and state -----------------------------------------------------------

void CloudSyncController::Hello() {
  if (shut_down_ || !loaded_ || !shared_->loaded() || hello_in_flight_) {
    return;
  }
  hello_in_flight_ = true;
  retry_timer_.Stop();
  pb::HelloRequest request;
  request.set_app_id(kBezelAppId);
  client_->Hello(request, base::BindOnce(&CloudSyncController::OnHello,
                                         weak_factory_.GetWeakPtr(),
                                         pairing_generation_));
}

void CloudSyncController::RetryHelloIn(base::TimeDelta delay) {
  retry_timer_.Start(FROM_HERE, delay,
                     base::BindOnce(&CloudSyncController::Hello,
                                    weak_factory_.GetWeakPtr()));
}

void CloudSyncController::OnHello(int generation,
                                  Status status,
                                  pb::HelloReply reply) {
  hello_in_flight_ = false;
  if (generation != pairing_generation_) {
    // Sent before the pairing changed (e.g. without the new token): its
    // "paired" is stale. Ask again with the current one.
    Hello();
    return;
  }
  if (!status.ok()) {
    watch_.reset();
    if (status.code == Code::kUnimplemented) {
      state_.phase = Phase::kIncompatible;
      state_.message = "Update the browser or CloudSync: " + status.message;
      RetryHelloIn(kIncompatibleRetry);
    } else {
      // Not installed or not running: stay standalone, look again later.
      state_.phase = Phase::kNotRunning;
      RetryHelloIn(kNotRunningRetry);
    }
    Notify();
    return;
  }
  backoff_ = base::TimeDelta();
  if (reply.api_version() != static_cast<uint32_t>(kConnectorApiVersion)) {
    state_.phase = Phase::kIncompatible;
    state_.message = "CloudSync speaks another connector API version.";
    RetryHelloIn(kIncompatibleRetry);
    Notify();
    return;
  }

  state_.phase = PhaseOf(reply.state());
  state_.paired = reply.paired();
  state_.vault_id = reply.vault_id();
  state_.device_name = reply.device_name();
  state_.core_version = reply.core_version();
  state_.passwords_scope =
      std::ranges::contains(reply.scopes(), std::string(kPasswordsScope));

  if (state_.paired) {
    shared_->SetScopes(
        std::vector<std::string>(reply.scopes().begin(), reply.scopes().end()));
  } else if (shared_->data().paired && state_.phase != Phase::kNoVault) {
    HandleUnpaired(
        "The pairing with CloudSync was removed. Switch sync on again to "
        "pair.");
    return;
  }

  if (saved_.enabled && state_.paired && !saved_.vault_id.empty() &&
      !reply.vault_id().empty() && saved_.vault_id != reply.vault_id()) {
    // Never merge across vaults (§ 4).
    StopReplication();
    state_.message =
        "CloudSync has another vault open. Switch sync on again to use it.";
    state_.enabled = false;
  }

  if (state_.paired && !watch_) {
    watch_ = client_->Watch(this);
  }
  MaybeStartReplication();
  if (state_.phase == Phase::kReady && replicator_ &&
      replicator_->running()) {
    replicator_->OnWritable();
  }
  Notify();
}

void CloudSyncController::MaybeStartReplication() {
  const bool usable =
      state_.phase == Phase::kReady || state_.phase == Phase::kOffline;
  if (!saved_.enabled || !state_.enabled || !state_.paired || !usable ||
      (replicator_ && replicator_->running()) || slot_in_flight_ ||
      app_key_in_flight_) {
    return;
  }
  if (state_.phase != Phase::kReady && saved_.slot_id.empty()) {
    return;  // A new slot needs a write: wait for READY.
  }
  ResolveSlot();
}

// --- profile slots -------------------------------------------------------------

void CloudSyncController::ResolveSlot() {
  slot_in_flight_ = true;
  pb::GetRequest request;
  request.set_key(kProfilesKey);
  client_->Get(request, base::BindOnce(&CloudSyncController::OnSlotsRead,
                                       weak_factory_.GetWeakPtr()));
}

void CloudSyncController::OnSlotsRead(Status status, pb::Item item) {
  if (!status.ok()) {
    slot_in_flight_ = false;
    if (status.code == Code::kUnauthenticated) {
      HandleUnpaired("CloudSync no longer knows this browser.");
      return;
    }
    state_.message =
        "Cannot read the profile list from CloudSync: " + status.ToString();
    Notify();
    return;
  }
  slots_revision_ = item.revision();
  std::vector<records::ProfileSlots> versions;
  for (const pb::Version& version : item.versions()) {
    records::ProfileSlots slots;
    if (slots.ParseFromString(version.data())) {
      versions.push_back(std::move(slots));
    }
  }
  records::ProfileSlots slots = MergeSlots(versions);
  bool rename = false;
  std::string slot_id = ChooseSlot(slots, saved_.slot_id, saved_.slot_name,
                                   state_.profile_name, &rename);
  const int64_t now = base::Time::Now().InMillisecondsSinceUnixEpoch();
  const bool create = slot_id.empty();
  bool write = item.versions_size() > 1;  // Merge several versions.
  if (create) {
    records::ProfileSlot* slot = slots.add_slots();
    slot_id = NewSlotId();
    slot->set_id(slot_id);
    slot->set_name(state_.profile_name);
    slot->set_created_ms(now);
    slot->set_renamed_ms(now);
    write = true;
  } else if (rename) {
    for (records::ProfileSlot& slot : *slots.mutable_slots()) {
      if (slot.id() == slot_id) {
        slot.set_name(state_.profile_name);
        slot.set_renamed_ms(now);
      }
    }
    write = true;
  }
  if (!write) {
    slot_in_flight_ = false;
    UseSlot(slot_id, slots);
    return;
  }
  if (state_.phase != Phase::kReady) {
    // Offline: writes fail. An existing slot can be used now; the rename or
    // merge is written once CloudSync is READY (next ResolveSlot).
    slot_in_flight_ = false;
    if (!create) {
      UseSlot(slot_id, slots);
    }
    return;
  }
  pb::PutRequest put;
  put.set_key(kProfilesKey);
  put.set_data(slots.SerializeAsString());
  put.set_base(slots_revision_);
  client_->Put(put, base::BindOnce(&CloudSyncController::OnSlotsWritten,
                                   weak_factory_.GetWeakPtr(), slot_id));
}

void CloudSyncController::OnSlotsWritten(std::string slot_id,
                                         Status status,
                                         pb::Item item) {
  slot_in_flight_ = false;
  if (status.code == Code::kAborted) {
    ResolveSlot();  // Another device changed the list: read and decide again.
    return;
  }
  if (!status.ok()) {
    state_.message =
        "Cannot write the profile list to CloudSync: " + status.ToString();
    Notify();
    return;
  }
  slots_revision_ = item.revision();
  records::ProfileSlots slots;
  if (item.versions_size() == 1) {
    slots.ParseFromString(item.versions(0).data());
  }
  UseSlot(slot_id, slots);
}

void CloudSyncController::UseSlot(const std::string& slot_id,
                                  const records::ProfileSlots& slots) {
  const records::ProfileSlot* slot = FindSlot(slots, slot_id);
  state_.slot_id = slot_id;
  state_.slot_name = slot ? slot->name() : state_.profile_name;
  state_.slot_names.clear();
  const records::ProfileSlot* primary = nullptr;
  for (const records::ProfileSlot& other : slots.slots()) {
    state_.slot_names.push_back(other.name());
    if (!primary || OlderSlot(other, *primary)) {
      primary = &other;
    }
  }
  state_.primary_slot = primary && primary->id() == slot_id;

  if (saved_.slot_id != slot_id || saved_.slot_name != state_.profile_name) {
    saved_.slot_id = slot_id;
    saved_.slot_name = state_.profile_name;
    Save();
  }
  if (replicator_ && replicator_slot_ != slot_id) {
    // Joined another profile's slot: the local server is reset with that
    // slot's key and takes its data; local data merges in (like pairing).
    replicator_->Stop();
    replicator_.reset();
  }
  if ((!replicator_ || !replicator_->running()) && !app_key_in_flight_) {
    app_key_in_flight_ = true;
    client_->AppKey(base::BindOnce(&CloudSyncController::OnAppKey,
                                   weak_factory_.GetWeakPtr()));
  }
  Notify();
}

void CloudSyncController::SetProfileName(std::string name) {
  if (name == state_.profile_name) {
    return;
  }
  state_.profile_name = std::move(name);
  Notify();
  if (saved_.enabled && state_.paired && state_.phase == Phase::kReady &&
      !slot_in_flight_) {
    ResolveSlot();
  }
}

void CloudSyncController::OnAppKey(Status status, pb::AppKeyReply reply) {
  app_key_in_flight_ = false;
  if (!status.ok()) {
    if (status.code == Code::kUnauthenticated) {
      HandleUnpaired("CloudSync no longer knows this browser.");
      return;
    }
    state_.message = "Cannot get the key from CloudSync: " + status.message;
    Notify();
    return;
  }
  if (state_.slot_id.empty() || !saved_.enabled || !state_.enabled) {
    return;
  }
  if (saved_.vault_id.empty()) {
    saved_.vault_id = state_.vault_id;
  }
  saved_.key_id = reply.key_id();
  Save();
  state_.message.clear();
  if (!replicator_) {
    replicator_ = std::make_unique<Replicator>(
        client_.get(), backend_file_, shared_->data().device_id,
        kSlotPrefix + state_.slot_id + "/", refresh_);
    replicator_slot_ = state_.slot_id;
  }
  // The replicator resets the local server if its key is another one.
  replicator_->Start(KeystoreKeyFromAppKey(reply.key(), state_.slot_id));
  Notify();
}

// --- Watch ---------------------------------------------------------------------

void CloudSyncController::OnChange(const pb::Change& change) {
  switch (change.kind()) {
    case pb::Change::RECORD:
      if (replicator_) {
        replicator_->OnRecordChanged(change.key(), change.revision(),
                                     change.deleted());
      }
      break;
    case pb::Change::STATE:
    case pb::Change::SCOPES:
      Hello();
      break;
    case pb::Change::LOGIN:
      for (Observer& observer : observers_) {
        observer.OnCloudSyncLoginsChanged();
      }
      break;
    case pb::Change::UNPAIRED:
      HandleUnpaired("The pairing was revoked in CloudSync.");
      break;
    default:
      break;  // Unknown kinds are ignored (§ 11).
  }
}

void CloudSyncController::OnWatchEnd(Status status) {
  watch_.reset();
  if (shut_down_) {
    return;
  }
  if (status.code == Code::kUnauthenticated) {
    HandleUnpaired("CloudSync no longer knows this browser.");
    return;
  }
  // Reconnect with backoff, Hello, then the replicator catches up (§ 6.3).
  backoff_ = std::clamp(backoff_ * 2, kMinBackoff, kMaxBackoff);
  RetryHelloIn(backoff_);
  if (replicator_ && replicator_->running()) {
    // Changes are not queued while disconnected: read everything again.
    replicator_->Stop();
  }
}

// --- pairing -------------------------------------------------------------------

void CloudSyncController::EnableSync() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!loaded_ || state_.pairing) {
    return;
  }
  state_.message.clear();
  if (state_.paired) {
    // The device is paired already (by this or another profile).
    saved_.enabled = true;
    saved_.vault_id = state_.vault_id;
    state_.enabled = true;
    Save();
    shared_->SetProfileEnabled(profile_key_, true);
    Hello();
    Notify();
    return;
  }
  RequestPairing();
}

void CloudSyncController::RequestPairing() {
  pb::PairingRequest request;
  request.set_app_id(kBezelAppId);
  request.set_app_name("Enclave");
  request.set_description(description_);
  request.add_scopes(kPasswordsScope);
  state_.pairing = true;
  state_.pairing_code.clear();
  Notify();
  client_->RequestPairing(request,
                          base::BindOnce(&CloudSyncController::OnPairingTicket,
                                         weak_factory_.GetWeakPtr()));
}

void CloudSyncController::OnPairingTicket(Status status,
                                          pb::PairingTicket ticket) {
  if (!state_.pairing) {
    return;  // Cancelled.
  }
  if (!status.ok()) {
    state_.pairing = false;
    state_.message = status.transport
                         ? "CloudSync is not running."
                         : "CloudSync refused the request: " + status.message;
    Notify();
    return;
  }
  ticket_ = ticket.ticket();
  const std::string& code = ticket.code();
  state_.pairing_code =
      code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
  Notify();
  AwaitPairing();
}

void CloudSyncController::AwaitPairing() {
  pb::AwaitPairingRequest request;
  request.set_ticket(ticket_);
  request.set_wait_seconds(60);
  client_->AwaitPairing(request,
                        base::BindOnce(&CloudSyncController::OnPairingResult,
                                       weak_factory_.GetWeakPtr()));
}

void CloudSyncController::OnPairingResult(Status status,
                                          pb::PairingResult result) {
  if (!state_.pairing) {
    return;  // Cancelled.
  }
  if (!status.ok() && status.code != Code::kDeadlineExceeded) {
    state_.pairing = false;
    state_.pairing_code.clear();
    state_.message = "Pairing failed: " + status.message;
    Notify();
    return;
  }
  if (!status.ok() || result.status() == pb::PENDING) {
    AwaitPairing();
    return;
  }
  state_.pairing = false;
  state_.pairing_code.clear();
  switch (result.status()) {
    case pb::APPROVED:
      // This profile switches sync on; every profile shares the pairing.
      saved_.enabled = true;
      saved_.vault_id.clear();  // Taken from the next Hello.
      state_.enabled = true;
      Save();
      // Desktop: the token, returned once. Android: empty (Binder). Stored
      // before anything else notifies, so that no Hello goes out without it.
      shared_->SetPaired(result.token(),
                         std::vector<std::string>(result.scopes().begin(),
                                                  result.scopes().end()));
      shared_->SetProfileEnabled(profile_key_, true);
      break;
    case pb::DENIED:
      state_.message = "Pairing was denied in CloudSync.";
      break;
    default:
      state_.message = "The pairing request expired. Try again.";
      break;
  }
  Notify();
}

void CloudSyncController::CancelPairing() {
  state_.pairing = false;
  state_.pairing_code.clear();
  ticket_.clear();
  Notify();
}

// --- switching off ---------------------------------------------------------------

void CloudSyncController::DisableSync() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (replicator_ && replicator_->running()) {
    replicator_->FlushNow();
  }
  StopReplication();
  saved_.enabled = false;
  saved_.vault_id.clear();
  saved_.key_id.clear();
  // The slot is kept: switching on again uses the same profile in CloudSync.
  state_.enabled = false;
  Save();
  shared_->SetProfileEnabled(profile_key_, false);
  if (!shared_->OthersEnabled(profile_key_)) {
    // The last profile with sync on: give the device's pairing up.
    if (state_.paired) {
      client_->Unpair(base::DoNothingAs<void(Status, pb::UnpairReply)>());
    }
    watch_.reset();
    state_.paired = false;
    state_.passwords_scope = false;
    shared_->ClearPairing();
  }
  Notify();
}

void CloudSyncController::RemoveSyncedData(
    base::OnceCallback<void(Status)> done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!replicator_ || !state_.paired) {
    std::move(done).Run({Code::kFailedPrecondition, "not paired"});
    return;
  }
  StopReplication();
  replicator_->RemoveAllRecords(base::BindOnce(
      [](base::WeakPtr<CloudSyncController> self,
         base::OnceCallback<void(Status)> done, Status status) {
        if (self && status.ok()) {
          self->DisableSync();
        }
        std::move(done).Run(std::move(status));
      },
      weak_factory_.GetWeakPtr(), std::move(done)));
}

void CloudSyncController::HandleUnpaired(std::string message) {
  StopReplication();
  watch_.reset();
  saved_.enabled = false;
  state_.enabled = false;
  state_.paired = false;
  state_.passwords_scope = false;
  state_.message = std::move(message);
  Save();
  // Every profile of this device loses the pairing with it.
  shared_->ClearPairing();
  Notify();
}

void CloudSyncController::StopReplication() {
  if (replicator_) {
    replicator_->Stop();
  }
}

// --- observers -------------------------------------------------------------------

bool CloudSyncController::CanUsePasswords() const {
  return state_.enabled && state_.paired && state_.passwords_scope &&
         !state_.slot_id.empty() &&
         (state_.phase == Phase::kReady || state_.phase == Phase::kOffline);
}

void CloudSyncController::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void CloudSyncController::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void CloudSyncController::Notify() {
  if (replicator_) {
    state_.replication = replicator_->stats();
  }
  for (Observer& observer : observers_) {
    observer.OnCloudSyncStateChanged();
  }
}

base::DictValue CloudSyncController::StateAsDict() const {
  base::DictValue dict;
  dict.Set("phase", PhaseName(state_.phase));
  dict.Set("enabled", state_.enabled);
  dict.Set("paired", state_.paired);
  dict.Set("passwords", state_.passwords_scope);
  dict.Set("pairing", state_.pairing);
  dict.Set("pairingCode", state_.pairing_code);
  dict.Set("message", state_.message);
  dict.Set("deviceName", state_.device_name);
  dict.Set("coreVersion", state_.core_version);
  dict.Set("profileName", state_.profile_name);
  dict.Set("slotName", state_.slot_name);
  dict.Set("primarySlot", state_.primary_slot);
  base::ListValue slot_names;
  for (const std::string& name : state_.slot_names) {
    slot_names.Append(name);
  }
  dict.Set("slotNames", std::move(slot_names));
  dict.Set("replicating", replicator_ && replicator_->running());
  const Replicator::Stats& stats =
      replicator_ ? replicator_->stats() : state_.replication;
  dict.Set("records", static_cast<int>(stats.records + stats.other_records));
  dict.Set("profileRecords", static_cast<int>(stats.records));
  dict.Set("recordLimit", static_cast<int>(Replicator::kMaxRecords));
  dict.Set("bytes", static_cast<double>(stats.bytes));
  dict.Set("lastWrite", stats.last_write.is_null()
                            ? 0.0
                            : stats.last_write.InMillisecondsFSinceUnixEpoch());
  dict.Set("error", stats.error);
  return dict;
}

}  // namespace bezel::cloudsync
