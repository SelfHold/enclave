// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_PASSWORD_CONVERSION_H_
#define CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_PASSWORD_CONVERSION_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "components/bezel_cloudsync/proto/connector.pb.h"
#include "components/password_manager/core/browser/password_store/stored_credential.h"

// Pure helpers of BezelPasswordMirror: StoredCredential <-> CloudSync Login
// (CONNECTOR.md § 12.1) and the merge decisions. No profile, no IO.
namespace bezel::password_sync {

using CloudLogin = ::cloudsync::connector::v1::Login;

// Prefix of the Chromium-only fields kept in Login.extra.
inline constexpr char kExtraPrefix[] = "bezel.";
// Login.extra key naming the profile slot a login belongs to. Logins
// without it (made in CloudSync's UI or by other apps) belong to the primary
// profile.
inline constexpr char kProfileExtraKey[] = "bezel.profile";
// CloudSync's limit on Login.extra, keys + values.
inline constexpr size_t kMaxExtraBytes = 16 * 1024;

// The password manager's own unique key (StoredCredentialUniqueKey):
// signon_realm, url, username_element, username_value, password_element.
// The login DB holds at most one credential per key.
using LoginKey = std::array<std::string, 5>;
LoginKey KeyOf(const password_manager::StoredCredential& cred);

// The login DB accepts a credential with this key: a signon_realm and a
// valid url (LoginDatabase's DoesMatchConstraints).
bool StorableKey(const LoginKey& key);

// 0 for a null time, and back.
int64_t ToMs(base::Time time);
base::Time FromMs(int64_t ms);

// The synced fields of `cred`, secrets included. id, revision, the times
// CloudSync keeps and device_name are left empty; created_ms is set (it is
// taken from the app for a new login). Chromium-only fields go into extra
// under "bezel.*", within kMaxExtraBytes. A non-empty `profile_tag` is
// written as extra[kProfileExtraKey] and is never trimmed away.
CloudLogin LoginFromCredential(const password_manager::StoredCredential& cred,
                               std::string_view profile_tag = {});

// extra[kProfileExtraKey], "" when untagged. Only known with data (GetLogins).
std::string ProfileTagOf(const CloudLogin& login);

// Whether the profile with slot `slot` mirrors a login tagged `tag`: its own
// tag, or no tag when it is the primary profile. Never with an empty slot.
bool OwnsLogin(std::string_view tag, std::string_view slot, bool primary);
bool OwnsLogin(const CloudLogin& login, std::string_view slot, bool primary);

// A credential for the login DB from a CloudSync login. Fields CloudSync does
// not carry keep their defaults; see KeepLocalOnlyFields.
password_manager::StoredCredential CredentialFromLogin(const CloudLogin& login);

// Copies into `out` (made by CredentialFromLogin) what only this device has:
// use counters, form data when the login carried none, alternative usernames,
// store flags, and the password issues when the password did not change and
// the login carried none.
void KeepLocalOnlyFields(const password_manager::StoredCredential& local,
                         password_manager::StoredCredential& out);

// Keeps the extra keys of other apps (not "bezel.*") from `remote` in `out`,
// as long as the total stays within kMaxExtraBytes. Never removes the
// profile tag of `out`.
void KeepForeignExtra(const CloudLogin& remote, CloudLogin& out);

// Hex SHA-256 of the synced, non-secret content of `cred`: everything
// LoginFromCredential sends except the password, the times, and the form
// data (which changes with every visit of the form). The password is left
// out so that the mapping file never holds a password verifier; password
// changes are seen through date_password_modified and the store's own
// "password changed" flag.
std::string ContentHash(const password_manager::StoredCredential& cred);

// Same synced content, password included (in memory only).
bool SameContent(const password_manager::StoredCredential& a,
                 const password_manager::StoredCredential& b);

// Only the use counters differ: times_used_in_html_form or date_last_used.
bool UseCountersAdvanced(const password_manager::StoredCredential& cred,
                         int64_t known_used_ms,
                         int known_times_used);

// Both sides changed the same login: true when the local copy wins. When the
// passwords differ, the later password change wins (local
// date_password_modified vs. password_modified_ms); otherwise the later
// change (`local_change_ms`, when the change was seen, or the local dates vs.
// modified_ms). A tie goes to CloudSync, so every device decides alike.
bool LocalWins(const password_manager::StoredCredential& local,
               int64_t local_change_ms,
               const CloudLogin& remote);

}  // namespace bezel::password_sync

#endif  // CHROME_BROWSER_BEZEL_CLOUDSYNC_BEZEL_PASSWORD_CONVERSION_H_
