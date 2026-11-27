// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/bezel/cloudsync/bezel_password_conversion.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "components/autofill/core/common/form_data.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_string.h"
#include "crypto/hash.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace bezel::password_sync {

namespace {

using password_manager::InsecureType;
using password_manager::InsecurityMetadata;
using password_manager::PasswordForm;
using password_manager::PasswordNote;
using password_manager::StoredCredential;

// Keys in Login.extra. Short: they count towards the 16 KiB.
constexpr char kScheme[] = "bezel.scheme";
constexpr char kType[] = "bezel.type";
constexpr char kSubmit[] = "bezel.submit";
constexpr char kChangeUrl[] = "bezel.chpw";
constexpr char kIcon[] = "bezel.icon";
constexpr char kGeneration[] = "bezel.gen";
constexpr char kSkipZeroClick[] = "bezel.szc";
constexpr char kActor[] = "bezel.actor";
constexpr char kIssues[] = "bezel.issues";
constexpr char kNotes[] = "bezel.notes";
constexpr char kShare[] = "bezel.share";
// Pickled autofill::FormData (as the login DB keeps it). Dropped first when
// the extra does not fit, and left out of ContentHash.
constexpr char kForm[] = "bezel.form";

std::string MsString(base::Time time) {
  return base::NumberToString(ToMs(time));
}

int64_t ParseMs(const base::Value* value) {
  int64_t ms = 0;
  if (value && value->is_string()) {
    base::StringToInt64(*value->GetIfString(), &ms);
  }
  return ms;
}

std::string ToJson(base::ValueView value) {
  return base::WriteJson(value).value_or(std::string());
}

// Every enum used here starts at 0 and has kMaxValue.
template <typename Enum>
std::optional<Enum> ParseEnum(const std::string& text) {
  int value = 0;
  if (!base::StringToInt(text, &value) || value < 0 ||
      value > static_cast<int>(Enum::kMaxValue)) {
    return std::nullopt;
  }
  return static_cast<Enum>(value);
}

size_t ExtraSize(const CloudLogin& login) {
  size_t size = 0;
  for (const auto& [key, value] : login.extra()) {
    size += key.size() + value.size();
  }
  return size;
}

std::map<std::string, std::string> ChromiumExtra(const StoredCredential& c) {
  std::map<std::string, std::string> extra;
  if (c.scheme != PasswordForm::Scheme::kHtml) {
    extra[kScheme] = base::NumberToString(static_cast<int>(c.scheme));
  }
  if (c.type != PasswordForm::Type::kFormSubmission) {
    extra[kType] = base::NumberToString(static_cast<int>(c.type));
  }
  if (!c.submit_element.empty()) {
    extra[kSubmit] = base::UTF16ToUTF8(c.submit_element);
  }
  if (c.change_password_url.is_valid()) {
    extra[kChangeUrl] = c.change_password_url.spec();
  }
  if (c.icon_url.is_valid()) {
    extra[kIcon] = c.icon_url.spec();
  }
  if (c.generation_upload_status !=
      PasswordForm::GenerationUploadStatus::kNoSignalSent) {
    extra[kGeneration] =
        base::NumberToString(static_cast<int>(c.generation_upload_status));
  }
  if (c.skip_zero_click) {
    extra[kSkipZeroClick] = "1";
  }
  if (c.actor_login_approved) {
    extra[kActor] = "1";
  }
  if (!c.password_issues.empty()) {
    // {"<type>": ["<create ms>", muted, notify]}
    base::DictValue issues;
    for (const auto& [type, meta] : c.password_issues) {
      base::ListValue item;
      item.Append(MsString(meta.create_time));
      item.Append(meta.is_muted.value());
      item.Append(meta.trigger_notification_from_backend.value());
      issues.Set(base::NumberToString(static_cast<int>(type)),
                 std::move(item));
    }
    extra[kIssues] = ToJson(issues);
  }
  // The unnamed note is Login.notes; named ones (the password change backup)
  // travel here: [["<name>", "<value>", "<created ms>", hide]].
  base::ListValue notes;
  for (const PasswordNote& note : c.notes) {
    if (note.unique_display_name.empty()) {
      continue;
    }
    base::ListValue item;
    item.Append(note.unique_display_name);
    item.Append(note.value);
    item.Append(MsString(note.date_created));
    item.Append(note.hide_by_default);
    notes.Append(std::move(item));
  }
  if (!notes.empty()) {
    extra[kNotes] = ToJson(notes);
  }
  if (!c.sender_email.empty() || !c.sender_name.empty() ||
      !c.date_received.is_null()) {
    base::DictValue share;
    share.Set("e", c.sender_email);
    share.Set("n", c.sender_name);
    if (c.sender_profile_image_url.is_valid()) {
      share.Set("i", c.sender_profile_image_url.spec());
    }
    share.Set("d", c.sharing_notification_displayed);
    share.Set("r", MsString(c.date_received));
    extra[kShare] = ToJson(share);
  }
  if (!c.form_data.fields().empty()) {
    base::Pickle pickle;
    autofill::SerializeFormData(c.form_data, &pickle);
    base::span<const uint8_t> bytes = pickle.AsBytes();
    extra[kForm] = std::string(bytes.begin(), bytes.end());
  }
  return extra;
}

void ParseChromiumExtra(const CloudLogin& login, StoredCredential& c) {
  const auto& extra = login.extra();
  auto find = [&extra](const char* key) -> const std::string* {
    auto it = extra.find(std::string(key));
    return it == extra.end() ? nullptr : &it->second;
  };
  if (const std::string* v = find(kScheme)) {
    c.scheme = ParseEnum<PasswordForm::Scheme>(*v).value_or(
        PasswordForm::Scheme::kHtml);
  }
  if (const std::string* v = find(kType)) {
    c.type = ParseEnum<PasswordForm::Type>(*v).value_or(
        PasswordForm::Type::kFormSubmission);
  }
  if (const std::string* v = find(kSubmit)) {
    c.submit_element = base::UTF8ToUTF16(*v);
  }
  if (const std::string* v = find(kChangeUrl)) {
    c.change_password_url = GURL(*v);
  }
  if (const std::string* v = find(kIcon)) {
    c.icon_url = GURL(*v);
  }
  if (const std::string* v = find(kGeneration)) {
    c.generation_upload_status =
        ParseEnum<PasswordForm::GenerationUploadStatus>(*v).value_or(
            PasswordForm::GenerationUploadStatus::kNoSignalSent);
  }
  c.skip_zero_click = find(kSkipZeroClick) != nullptr;
  c.actor_login_approved = find(kActor) != nullptr;
  if (const std::string* v = find(kIssues)) {
    std::optional<base::DictValue> issues =
        base::JSONReader::ReadDict(*v, base::JSON_PARSE_RFC);
    if (issues) {
      for (const auto [key, value] : *issues) {
        std::optional<InsecureType> type = ParseEnum<InsecureType>(key);
        const base::ListValue* item = value.GetIfList();
        if (!type || !item || item->size() != 3) {
          continue;
        }
        c.password_issues[*type] = InsecurityMetadata(
            FromMs(ParseMs(&(*item)[0])),
            password_manager::IsMuted((*item)[1].GetIfBool().value_or(false)),
            password_manager::TriggerBackendNotification(
                (*item)[2].GetIfBool().value_or(false)));
      }
    }
  }
  if (const std::string* v = find(kNotes)) {
    std::optional<base::ListValue> notes =
        base::JSONReader::ReadList(*v, base::JSON_PARSE_RFC);
    if (notes) {
      for (const base::Value& value : *notes) {
        const base::ListValue* item = value.GetIfList();
        if (!item || item->size() != 4 || !(*item)[0].is_string() ||
            !(*item)[1].is_string()) {
          continue;
        }
        c.notes.emplace_back(base::UTF8ToUTF16(*(*item)[0].GetIfString()),
                             base::UTF8ToUTF16(*(*item)[1].GetIfString()),
                             FromMs(ParseMs(&(*item)[2])),
                             (*item)[3].GetIfBool().value_or(false));
      }
    }
  }
  if (const std::string* v = find(kShare)) {
    std::optional<base::DictValue> share =
        base::JSONReader::ReadDict(*v, base::JSON_PARSE_RFC);
    if (share) {
      if (const std::string* e = share->FindString("e")) {
        c.sender_email = base::UTF8ToUTF16(*e);
      }
      if (const std::string* n = share->FindString("n")) {
        c.sender_name = base::UTF8ToUTF16(*n);
      }
      if (const std::string* i = share->FindString("i")) {
        c.sender_profile_image_url = GURL(*i);
      }
      c.sharing_notification_displayed = share->FindBool("d").value_or(false);
      c.date_received = FromMs(ParseMs(share->Find("r")));
    }
  }
  if (const std::string* v = find(kForm)) {
    base::PickleIterator iter =
        base::PickleIterator::WithData(base::as_byte_span(*v));
    autofill::FormData form;
    if (autofill::DeserializeFormData(&iter, &form)) {
      c.form_data = std::move(form);
    }
  }
}

void AppendField(std::string& out, std::string_view value) {
  out += base::NumberToString(value.size());
  out += ':';
  out += value;
  out += ';';
}

}  // namespace

LoginKey KeyOf(const StoredCredential& cred) {
  return {cred.signon_realm, cred.url.possibly_invalid_spec(),
          base::UTF16ToUTF8(cred.username_element),
          base::UTF16ToUTF8(cred.username_value),
          base::UTF16ToUTF8(cred.password_element)};
}

bool StorableKey(const LoginKey& key) {
  return !key[0].empty() && GURL(key[1]).is_valid();
}

int64_t ToMs(base::Time time) {
  return time.is_null() ? 0 : time.InMillisecondsSinceUnixEpoch();
}

base::Time FromMs(int64_t ms) {
  return ms == 0 ? base::Time() : base::Time::FromMillisecondsSinceUnixEpoch(ms);
}

CloudLogin LoginFromCredential(const StoredCredential& cred,
                               std::string_view profile_tag) {
  CloudLogin login;
  login.set_signon_realm(cred.signon_realm);
  login.set_url(cred.url.possibly_invalid_spec());
  login.set_action_url(cred.action.possibly_invalid_spec());
  login.set_username(base::UTF16ToUTF8(cred.username_value));
  login.set_password(base::UTF16ToUTF8(cred.password_value.value()));
  login.set_username_element(base::UTF16ToUTF8(cred.username_element));
  login.set_password_element(base::UTF16ToUTF8(cred.password_element));
  login.set_display_name(base::UTF16ToUTF8(cred.display_name));
  login.set_notes(base::UTF16ToUTF8(cred.GetPasswordNote()));
  if (cred.federation_origin.IsValid()) {
    login.set_federation_origin(cred.federation_origin.Serialize());
  }
  login.set_blocked(cred.blocked_by_user);
  login.set_created_ms(ToMs(cred.date_created));

  std::map<std::string, std::string> extra = ChromiumExtra(cred);
  // The profile tag is never dropped: its room is taken first.
  size_t size = profile_tag.empty()
                    ? 0
                    : std::string_view(kProfileExtraKey).size() +
                          profile_tag.size();
  for (const auto& [key, value] : extra) {
    if (key != kForm) {
      size += key.size() + value.size();
    }
  }
  auto form = extra.find(kForm);
  if (form != extra.end() &&
      size + form->first.size() + form->second.size() > kMaxExtraBytes) {
    extra.erase(form);
  }
  // Still too big (huge notes or issues): drop the largest optional parts,
  // then anything else of ours, until it fits.
  for (const char* key : {kNotes, kShare, kIssues}) {
    if (size <= kMaxExtraBytes) {
      break;
    }
    auto it = extra.find(key);
    if (it != extra.end()) {
      size -= it->first.size() + it->second.size();
      extra.erase(it);
    }
  }
  while (size > kMaxExtraBytes && !extra.empty()) {
    auto largest = std::ranges::max_element(extra, {}, [](const auto& item) {
      return item.first.size() + item.second.size();
    });
    size -= largest->first.size() + largest->second.size();
    extra.erase(largest);
  }
  for (auto& [key, value] : extra) {
    (*login.mutable_extra())[key] = std::move(value);
  }
  if (!profile_tag.empty()) {
    (*login.mutable_extra())[std::string(kProfileExtraKey)] =
        std::string(profile_tag);
  }
  return login;
}

std::string ProfileTagOf(const CloudLogin& login) {
  auto it = login.extra().find(std::string(kProfileExtraKey));
  return it == login.extra().end() ? std::string() : it->second;
}

bool OwnsLogin(std::string_view tag, std::string_view slot, bool primary) {
  if (slot.empty()) {
    return false;  // The profile's slot is not known yet.
  }
  return tag.empty() ? primary : tag == slot;
}

bool OwnsLogin(const CloudLogin& login, std::string_view slot, bool primary) {
  return OwnsLogin(ProfileTagOf(login), slot, primary);
}

StoredCredential CredentialFromLogin(const CloudLogin& login) {
  StoredCredential c;
  c.signon_realm = login.signon_realm();
  c.url = GURL(login.url().empty() ? login.signon_realm() : login.url());
  c.action = GURL(login.action_url());
  c.username_element = base::UTF8ToUTF16(login.username_element());
  c.password_element = base::UTF8ToUTF16(login.password_element());
  c.blocked_by_user = login.blocked();
  if (!c.blocked_by_user) {
    c.username_value = base::UTF8ToUTF16(login.username());
    c.password_value =
        password_manager::PasswordString(base::UTF8ToUTF16(login.password()));
  }
  c.display_name = base::UTF8ToUTF16(login.display_name());
  if (!login.federation_origin().empty()) {
    c.federation_origin =
        url::SchemeHostPort(GURL(login.federation_origin()));
  }
  c.date_created = login.created_ms() ? FromMs(login.created_ms())
                                      : (login.modified_ms()
                                             ? FromMs(login.modified_ms())
                                             : base::Time::Now());
  c.date_password_modified = login.password_modified_ms()
                                 ? FromMs(login.password_modified_ms())
                                 : c.date_created;
  // This device's own counters (TouchLogin), § 12.4.
  c.date_last_used = FromMs(login.last_used_ms());
  c.times_used_in_html_form = static_cast<int>(
      std::clamp<int64_t>(login.times_used(), 0, 1 << 30));
  c.in_store = PasswordForm::Store::kProfileStore;
  if (!login.notes().empty()) {
    c.notes.emplace_back(base::UTF8ToUTF16(login.notes()),
                         login.modified_ms() ? FromMs(login.modified_ms())
                                             : c.date_created);
  }
  ParseChromiumExtra(login, c);
  return c;
}

void KeepLocalOnlyFields(const StoredCredential& local, StoredCredential& out) {
  out.primary_key = local.primary_key;
  out.times_used_in_html_form = local.times_used_in_html_form;
  out.date_last_used = local.date_last_used;
  out.date_last_filled = local.date_last_filled;
  out.all_alternative_usernames = local.all_alternative_usernames;
  if (out.form_data.fields().empty()) {
    out.form_data = local.form_data;
  }
  out.in_store = local.in_store;
  out.moving_blocked_for_list = local.moving_blocked_for_list;
  out.previously_associated_sync_account_email =
      local.previously_associated_sync_account_email;
  out.keychain_identifier = local.keychain_identifier;
  out.affiliated_web_realm = local.affiliated_web_realm;
  out.app_display_name = local.app_display_name;
  out.app_icon_url = local.app_icon_url;
  out.match_type = local.match_type;
  if (out.password_issues.empty() &&
      out.password_value == local.password_value) {
    out.password_issues = local.password_issues;
  }
}

void KeepForeignExtra(const CloudLogin& remote, CloudLogin& out) {
  size_t foreign = 0;
  for (const auto& [key, value] : remote.extra()) {
    if (!base::StartsWith(key, kExtraPrefix)) {
      foreign += key.size() + value.size();
    }
  }
  if (foreign == 0) {
    return;
  }
  auto* extra = out.mutable_extra();
  if (ExtraSize(out) + foreign > kMaxExtraBytes) {
    extra->erase(std::string(kForm));
  }
  size_t size = ExtraSize(out);
  for (const auto& [key, value] : remote.extra()) {
    if (base::StartsWith(key, kExtraPrefix) ||
        size + key.size() + value.size() > kMaxExtraBytes) {
      continue;
    }
    (*extra)[key] = value;
    size += key.size() + value.size();
  }
}

std::string ContentHash(const StoredCredential& cred) {
  CloudLogin login = LoginFromCredential(cred);
  std::string text = "bezel-login-v1;";
  AppendField(text, login.signon_realm());
  AppendField(text, login.url());
  AppendField(text, login.action_url());
  AppendField(text, login.username());
  AppendField(text, login.username_element());
  AppendField(text, login.password_element());
  AppendField(text, login.display_name());
  AppendField(text, login.notes());
  AppendField(text, login.federation_origin());
  AppendField(text, login.blocked() ? "1" : "0");
  // Proto maps have no order: sort.
  std::map<std::string, std::string> extra;
  for (const auto& [key, value] : login.extra()) {
    if (key != kForm) {
      extra.emplace(key, value);
    }
  }
  for (const auto& [key, value] : extra) {
    AppendField(text, key);
    AppendField(text, value);
  }
  // `login` holds the password: wipe the copy before it goes away.
  login.mutable_password()->assign(login.password().size(), '\0');
  return base::HexEncodeLower(crypto::hash::Sha256(text));
}

bool SameContent(const StoredCredential& a, const StoredCredential& b) {
  return a.password_value == b.password_value &&
         ContentHash(a) == ContentHash(b);
}

bool UseCountersAdvanced(const StoredCredential& cred,
                         int64_t known_used_ms,
                         int known_times_used) {
  return cred.times_used_in_html_form != known_times_used ||
         ToMs(cred.date_last_used) > known_used_ms;
}

bool LocalWins(const StoredCredential& local,
               int64_t local_change_ms,
               const CloudLogin& remote) {
  const bool passwords_differ =
      !local.blocked_by_user && !remote.blocked() &&
      !(local.password_value == base::UTF8ToUTF16(remote.password()));
  if (passwords_differ) {
    return ToMs(local.date_password_modified) > remote.password_modified_ms();
  }
  const int64_t local_ms =
      std::max({local_change_ms, ToMs(local.date_password_modified),
                ToMs(local.date_created)});
  const int64_t remote_ms =
      std::max(remote.modified_ms(), remote.password_modified_ms());
  return local_ms > remote_ms;
}

}  // namespace bezel::password_sync
