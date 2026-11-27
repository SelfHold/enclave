// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/connector_address.h"

#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "net/base/ip_address.h"

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include "base/environment.h"
#include "base/nix/xdg_util.h"
#endif

namespace enclave::shard {

namespace {

constexpr std::string_view kUnixScheme = "unix://";
// connector.json is a few dozen bytes; anything big is not ours.
constexpr int64_t kMaxConnectorJsonSize = 4096;

std::optional<ConnectorAddress> ParseTcp(std::string_view address) {
  const size_t colon = address.rfind(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  std::string_view host = address.substr(0, colon);
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    host = host.substr(1, host.size() - 2);
  }
  net::IPAddress ip;
  int port = 0;
  if (!ip.AssignFromIPLiteral(host) || !ip.IsLoopback() ||
      !base::StringToInt(address.substr(colon + 1), &port) || port <= 0 ||
      port > 65535) {
    return std::nullopt;
  }
  ConnectorAddress result;
  result.kind = ConnectorAddress::Kind::kTcp;
  result.host = std::string(host);
  result.port = static_cast<uint16_t>(port);
  return result;
}

}  // namespace

std::optional<ConnectorAddress> ParseConnectorJson(std::string_view json) {
  std::optional<base::DictValue> dict =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!dict) {
    return std::nullopt;
  }
  const std::string* address = dict->FindString("address");
  const std::optional<int> api_version = dict->FindInt("api_version");
  if (!address || !api_version) {
    return std::nullopt;
  }

  std::optional<ConnectorAddress> result;
  if (address->starts_with(kUnixScheme)) {
    const std::string path = address->substr(kUnixScheme.size());
    if (path.empty() || path.front() != '/') {
      return std::nullopt;
    }
    result.emplace();
    result->kind = ConnectorAddress::Kind::kUnix;
    result->unix_path = path;
  } else {
    result = ParseTcp(*address);
  }
  if (result) {
    result->api_version = *api_version;
  }
  return result;
}

base::FilePath GetConnectorJsonPath() {
  base::FilePath dir;
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  dir = base::nix::GetXDGDirectory(env.get(), base::nix::kXdgConfigHomeEnvVar,
                                   base::nix::kDotConfigDir);
#elif BUILDFLAG(IS_WIN)
  if (!base::PathService::Get(base::DIR_ROAMING_APP_DATA, &dir)) {
    return {};
  }
#elif BUILDFLAG(IS_MAC)
  if (!base::PathService::Get(base::DIR_APP_DATA, &dir)) {
    return {};
  }
#else
  return {};  // Android binds to the Shard service instead.
#endif
  if (dir.empty()) {
    return {};
  }
  return dir.Append(FILE_PATH_LITERAL("selfhold"))
      .Append(FILE_PATH_LITERAL("shard"))
      .Append(FILE_PATH_LITERAL("connector.json"));
}

std::optional<ConnectorAddress> ReadConnectorJson(const base::FilePath& path) {
  std::string text;
  if (path.empty() ||
      !base::ReadFileToStringWithMaxSize(path, &text, kMaxConnectorJsonSize)) {
    return std::nullopt;
  }
  return ParseConnectorJson(text);
}

}  // namespace enclave::shard
