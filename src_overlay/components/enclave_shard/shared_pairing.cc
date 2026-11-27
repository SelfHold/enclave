// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/shared_pairing.h"

#include <optional>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "base/values.h"
#include "build/build_config.h"

namespace enclave::shard {

namespace {

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
#if BUILDFLAG(IS_POSIX)
  // Holds the pairing token: readable by the user only (CONNECTOR.md § 5).
  base::SetPosixFilePermissions(path, base::FILE_PERMISSION_READ_BY_USER |
                                          base::FILE_PERMISSION_WRITE_BY_USER);
#endif
}

std::vector<std::string> Strings(const base::ListValue* list) {
  std::vector<std::string> result;
  if (list) {
    for (const base::Value& value : *list) {
      if (value.is_string()) {
        result.push_back(value.GetString());
      }
    }
  }
  return result;
}

}  // namespace

SharedPairing::Data::Data() = default;
SharedPairing::Data::Data(const Data&) = default;
SharedPairing::Data& SharedPairing::Data::operator=(const Data&) = default;
SharedPairing::Data::~Data() = default;

SharedPairing::SharedPairing(const base::FilePath& file)
    : file_(file),
      file_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  file_runner_->PostTaskAndReplyWithResult(
      FROM_HERE, base::BindOnce(&ReadFile, file_),
      base::BindOnce(&SharedPairing::OnLoaded, weak_factory_.GetWeakPtr()));
}

SharedPairing::~SharedPairing() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void SharedPairing::OnLoaded(std::optional<std::string> text) {
  if (text) {
    std::optional<base::DictValue> dict =
        base::JSONReader::ReadDict(*text, base::JSON_PARSE_RFC);
    if (dict) {
      if (const std::string* token = dict->FindString("token")) {
        data_.token = *token;
      }
      data_.paired = dict->FindBool("paired").value_or(false);
      data_.scopes = Strings(dict->FindList("scopes"));
      if (const std::string* device = dict->FindString("device_id")) {
        data_.device_id = *device;
      }
      for (std::string& profile : Strings(dict->FindList("enabled_profiles"))) {
        data_.enabled_profiles.insert(std::move(profile));
      }
    }
  }
  loaded_ = true;
  if (data_.device_id.empty()) {
    data_.device_id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  }
  SaveAndNotify();
}

void SharedPairing::SetPaired(std::string token,
                              std::vector<std::string> scopes) {
  data_.token = std::move(token);
  data_.paired = true;
  data_.scopes = std::move(scopes);
  SaveAndNotify();
}

void SharedPairing::SetScopes(std::vector<std::string> scopes) {
  if (data_.scopes == scopes) {
    return;
  }
  data_.scopes = std::move(scopes);
  SaveAndNotify();
}

void SharedPairing::ClearPairing() {
  if (!data_.paired && data_.token.empty() && data_.enabled_profiles.empty()) {
    return;
  }
  data_.token.clear();
  data_.paired = false;
  data_.scopes.clear();
  data_.enabled_profiles.clear();
  SaveAndNotify();
}

void SharedPairing::SetProfileEnabled(const std::string& profile,
                                      bool enabled) {
  const bool changed = enabled ? data_.enabled_profiles.insert(profile).second
                               : data_.enabled_profiles.erase(profile) > 0;
  if (changed) {
    SaveAndNotify();
  }
}

bool SharedPairing::OthersEnabled(const std::string& profile) const {
  for (const std::string& other : data_.enabled_profiles) {
    if (other != profile) {
      return true;
    }
  }
  return false;
}

void SharedPairing::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SharedPairing::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SharedPairing::SaveAndNotify() {
  base::DictValue dict;
  dict.Set("token", data_.token);
  dict.Set("paired", data_.paired);
  base::ListValue scopes;
  for (const std::string& scope : data_.scopes) {
    scopes.Append(scope);
  }
  dict.Set("scopes", std::move(scopes));
  dict.Set("device_id", data_.device_id);
  base::ListValue profiles;
  for (const std::string& profile : data_.enabled_profiles) {
    profiles.Append(profile);
  }
  dict.Set("enabled_profiles", std::move(profiles));
  std::string text;
  base::JSONWriter::Write(dict, &text);
  file_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&WriteFile, file_, std::move(text)));
  for (Observer& observer : observers_) {
    observer.OnSharedPairingChanged();
  }
}

}  // namespace enclave::shard
