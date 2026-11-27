// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include <string>
#include <utility>
#include <vector>

#include "base/time/time.h"
#include "chrome/browser/enclave/shard/enclave_password_conversion.h"
#include "components/autofill/core/common/form_data.h"
#include "components/autofill/core/common/form_field_data.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_string.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace enclave::password_sync {
namespace {

using password_manager::InsecureType;
using password_manager::InsecurityMetadata;
using password_manager::PasswordForm;
using password_manager::PasswordNote;
using password_manager::PasswordString;
using password_manager::StoredCredential;

StoredCredential MakeCredential() {
  StoredCredential c;
  c.signon_realm = "https://example.com/";
  c.url = GURL("https://example.com/login");
  c.action = GURL("https://example.com/post");
  c.username_element = u"user";
  c.password_element = u"pass";
  c.submit_element = u"go";
  c.username_value = u"alice";
  c.password_value = PasswordString(u"s3cret");
  c.display_name = u"Alice";
  c.date_created = base::Time::FromMillisecondsSinceUnixEpoch(1'700'000'000'000);
  c.date_password_modified =
      base::Time::FromMillisecondsSinceUnixEpoch(1'700'000'100'000);
  c.scheme = PasswordForm::Scheme::kBasic;
  c.type = PasswordForm::Type::kGenerated;
  c.in_store = PasswordForm::Store::kProfileStore;
  c.notes.emplace_back(u"a note", c.date_created);
  c.notes.emplace_back(std::u16string(PasswordNote::kPasswordChangeBackupNoteName),
                       u"old", c.date_created, /*hide_by_default=*/true);
  c.password_issues[InsecureType::kLeaked] =
      InsecurityMetadata(c.date_created, password_manager::IsMuted(true),
                         password_manager::TriggerBackendNotification(false));
  return c;
}

TEST(EnclavePasswordConversionTest, KeyIsTheUniqueKey) {
  StoredCredential c = MakeCredential();
  LoginKey key = KeyOf(c);
  EXPECT_EQ(key[0], "https://example.com/");
  EXPECT_EQ(key[1], "https://example.com/login");
  EXPECT_EQ(key[2], "user");
  EXPECT_EQ(key[3], "alice");
  EXPECT_EQ(key[4], "pass");
  EXPECT_TRUE(StorableKey(key));
  key[1] = "";
  EXPECT_FALSE(StorableKey(key));
}

TEST(EnclavePasswordConversionTest, RoundTrip) {
  StoredCredential c = MakeCredential();
  CloudLogin login = LoginFromCredential(c);
  EXPECT_EQ(login.username(), "alice");
  EXPECT_EQ(login.password(), "s3cret");
  EXPECT_EQ(login.notes(), "a note");
  EXPECT_EQ(login.created_ms(), 1'700'000'000'000);
  for (const auto& [key, value] : login.extra()) {
    EXPECT_EQ(key.rfind(kExtraPrefix, 0), 0u) << key;
  }
  login.set_password_modified_ms(1'700'000'100'000);

  StoredCredential back = CredentialFromLogin(login);
  EXPECT_EQ(KeyOf(back), KeyOf(c));
  EXPECT_TRUE(back.password_value == c.password_value);
  EXPECT_EQ(back.scheme, PasswordForm::Scheme::kBasic);
  EXPECT_EQ(back.type, PasswordForm::Type::kGenerated);
  EXPECT_EQ(back.submit_element, u"go");
  EXPECT_EQ(back.GetPasswordNote(), u"a note");
  EXPECT_EQ(back.GetPasswordBackup(), u"old");
  ASSERT_EQ(back.password_issues.size(), 1u);
  EXPECT_TRUE(back.password_issues[InsecureType::kLeaked].is_muted.value());
  EXPECT_EQ(back.date_password_modified, c.date_password_modified);
  EXPECT_TRUE(SameContent(c, back));
}

TEST(EnclavePasswordConversionTest, BlockedAndFederated) {
  StoredCredential blocked;
  blocked.signon_realm = "https://blocked.example/";
  blocked.url = GURL("https://blocked.example/");
  blocked.blocked_by_user = true;
  CloudLogin login = LoginFromCredential(blocked);
  EXPECT_TRUE(login.blocked());
  EXPECT_TRUE(CredentialFromLogin(login).blocked_by_user);

  StoredCredential federated = MakeCredential();
  federated.password_value = PasswordString();
  federated.federation_origin =
      url::SchemeHostPort(GURL("https://accounts.google.com"));
  login = LoginFromCredential(federated);
  EXPECT_EQ(login.federation_origin(), "https://accounts.google.com");
  EXPECT_EQ(CredentialFromLogin(login).federation_origin,
            federated.federation_origin);
}

TEST(EnclavePasswordConversionTest, HashLeavesOutPasswordTimesAndForm) {
  StoredCredential a = MakeCredential();
  StoredCredential b = MakeCredential();
  b.password_value = PasswordString(u"other");
  b.date_last_used = base::Time::Now();
  b.times_used_in_html_form = 7;
  autofill::FormFieldData field;
  field.set_name(u"user");
  autofill::FormData form;
  form.set_url(GURL("https://example.com/login"));
  form.set_fields({field});
  b.form_data = form;
  EXPECT_EQ(ContentHash(a), ContentHash(b));
  EXPECT_FALSE(SameContent(a, b));
  EXPECT_EQ(ContentHash(a).find("s3cret"), std::string::npos);

  b.display_name = u"Bob";
  EXPECT_NE(ContentHash(a), ContentHash(b));
}

TEST(EnclavePasswordConversionTest, FormDataTravelsWhenItFits) {
  StoredCredential c = MakeCredential();
  autofill::FormFieldData field;
  field.set_name(u"user");
  autofill::FormData form;
  form.set_url(GURL("https://example.com/login"));
  form.set_fields({field});
  c.form_data = form;
  StoredCredential back = CredentialFromLogin(LoginFromCredential(c));
  ASSERT_EQ(back.form_data.fields().size(), 1u);
  EXPECT_EQ(back.form_data.fields()[0].name(), u"user");

  // A note too big for the extra: dropped, the rest still fits.
  c.notes.emplace_back(u"huge", std::u16string(20000, u'x'), c.date_created,
                       false);
  CloudLogin login = LoginFromCredential(c);
  size_t size = 0;
  for (const auto& [key, value] : login.extra()) {
    size += key.size() + value.size();
  }
  EXPECT_LE(size, kMaxExtraBytes);
}

TEST(EnclavePasswordConversionTest, KeepsLocalOnlyFields) {
  StoredCredential local = MakeCredential();
  local.times_used_in_html_form = 4;
  local.date_last_used = base::Time::Now();
  CloudLogin login = LoginFromCredential(local);
  login.clear_extra();
  StoredCredential merged = CredentialFromLogin(login);
  KeepLocalOnlyFields(local, merged);
  EXPECT_EQ(merged.times_used_in_html_form, 4);
  EXPECT_EQ(merged.date_last_used, local.date_last_used);
  // Same password, no issues sent: the local ones stay.
  EXPECT_EQ(merged.password_issues.size(), 1u);
}

TEST(EnclavePasswordConversionTest, KeepsOtherAppsExtra) {
  CloudLogin remote = LoginFromCredential(MakeCredential());
  (*remote.mutable_extra())["other.app"] = "v";
  (*remote.mutable_extra())["enclave.scheme"] = "3";
  CloudLogin out = LoginFromCredential(MakeCredential());
  KeepForeignExtra(remote, out);
  EXPECT_EQ(out.extra().at("other.app"), "v");
  EXPECT_EQ(out.extra().at("enclave.scheme"), "1");  // Ours, not theirs.
}

TEST(EnclavePasswordConversionTest, UseCounters) {
  StoredCredential c = MakeCredential();
  c.times_used_in_html_form = 2;
  c.date_last_used = base::Time::FromMillisecondsSinceUnixEpoch(5000);
  EXPECT_FALSE(UseCountersAdvanced(c, 5000, 2));
  EXPECT_TRUE(UseCountersAdvanced(c, 4000, 2));
  EXPECT_TRUE(UseCountersAdvanced(c, 5000, 1));
}

TEST(EnclavePasswordConversionTest, NewerWins) {
  StoredCredential local = MakeCredential();  // Password changed at ...100'000.
  CloudLogin remote = LoginFromCredential(local);
  remote.set_password("different");
  remote.set_password_modified_ms(1'700'000'200'000);
  EXPECT_FALSE(LocalWins(local, 0, remote));
  remote.set_password_modified_ms(1'700'000'050'000);
  EXPECT_TRUE(LocalWins(local, 0, remote));

  // Same password: the later change of anything else.
  remote.set_password("s3cret");
  remote.set_modified_ms(1'700'000'300'000);
  EXPECT_FALSE(LocalWins(local, 1'700'000'200'000, remote));
  EXPECT_TRUE(LocalWins(local, 1'700'000'400'000, remote));
  // A tie goes to Shard.
  EXPECT_FALSE(LocalWins(local, 1'700'000'300'000, remote));
}

TEST(EnclavePasswordConversionTest, OwnershipTaggedOwn) {
  CloudLogin login = LoginFromCredential(MakeCredential(), "slot-a");
  EXPECT_EQ(ProfileTagOf(login), "slot-a");
  EXPECT_TRUE(OwnsLogin(login, "slot-a", /*primary=*/false));
  EXPECT_TRUE(OwnsLogin(login, "slot-a", /*primary=*/true));
}

TEST(EnclavePasswordConversionTest, OwnershipTaggedOther) {
  CloudLogin login = LoginFromCredential(MakeCredential(), "slot-b");
  EXPECT_FALSE(OwnsLogin(login, "slot-a", /*primary=*/false));
  EXPECT_FALSE(OwnsLogin(login, "slot-a", /*primary=*/true));
}

TEST(EnclavePasswordConversionTest, OwnershipUntaggedPrimary) {
  CloudLogin login = LoginFromCredential(MakeCredential());
  EXPECT_EQ(ProfileTagOf(login), "");
  EXPECT_TRUE(OwnsLogin(login, "slot-a", /*primary=*/true));
}

TEST(EnclavePasswordConversionTest, OwnershipUntaggedNotPrimary) {
  CloudLogin login = LoginFromCredential(MakeCredential());
  EXPECT_FALSE(OwnsLogin(login, "slot-a", /*primary=*/false));
}

TEST(EnclavePasswordConversionTest, OwnershipNeedsASlot) {
  EXPECT_FALSE(OwnsLogin("", "", /*primary=*/true));
  EXPECT_FALSE(OwnsLogin("slot-a", "", /*primary=*/true));
}

TEST(EnclavePasswordConversionTest, ProfileTagIsNeverTrimmed) {
  StoredCredential c = MakeCredential();
  c.notes.emplace_back(u"huge", std::u16string(20000, u'x'), c.date_created,
                       false);
  c.submit_element = std::u16string(17000, u's');  // Too big on its own.
  CloudLogin login = LoginFromCredential(c, "slot-a");
  EXPECT_EQ(ProfileTagOf(login), "slot-a");
  size_t size = 0;
  for (const auto& [key, value] : login.extra()) {
    size += key.size() + value.size();
  }
  EXPECT_LE(size, kMaxExtraBytes);

  // Other apps' keys do not push it out either.
  CloudLogin remote;
  (*remote.mutable_extra())["other.app"] = std::string(kMaxExtraBytes, 'o');
  KeepForeignExtra(remote, login);
  EXPECT_EQ(ProfileTagOf(login), "slot-a");
}

TEST(EnclavePasswordConversionTest, ProfileTagStaysOutOfTheHash) {
  StoredCredential c = MakeCredential();
  CloudLogin tagged = LoginFromCredential(c, "slot-a");
  StoredCredential back = CredentialFromLogin(tagged);
  KeepLocalOnlyFields(c, back);
  EXPECT_EQ(ContentHash(c), ContentHash(back));
}

}  // namespace
}  // namespace enclave::password_sync
