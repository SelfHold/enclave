// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_ENCLAVE_SHARD_CONNECTOR_ADDRESS_H_
#define COMPONENTS_ENCLAVE_SHARD_CONNECTOR_ADDRESS_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "base/files/file_path.h"

namespace enclave::shard {

// The only API version this client speaks (CONNECTOR.md § 3.1).
inline constexpr int kConnectorApiVersion = 1;

// Where shardd serves the Connector API, from connector.json.
struct ConnectorAddress {
  enum class Kind { kUnix, kTcp };

  Kind kind = Kind::kUnix;
  std::string unix_path;  // kUnix: absolute path of the socket.
  std::string host;       // kTcp: always a loopback IP literal.
  uint16_t port = 0;      // kTcp.
  int api_version = 0;

  bool operator==(const ConnectorAddress&) const = default;
};

// Parses connector.json. Accepts `unix:///absolute/path` and `host:port` on a
// loopback address only (§ 10: the daemon never listens elsewhere, so anything
// else is not Shard). Returns nullopt for anything else.
std::optional<ConnectorAddress> ParseConnectorJson(std::string_view json);

// Path of connector.json for this OS (§ 3.1), or empty if it cannot be found.
// Linux: $XDG_CONFIG_HOME/selfhold/shard, Windows: %AppData%\selfhold\shard,
// macOS: ~/Library/Application Support/selfhold/shard.
base::FilePath GetConnectorJsonPath();

// Reads and parses `path`. Blocking: call it on a sequence that may block.
std::optional<ConnectorAddress> ReadConnectorJson(const base::FilePath& path);

}  // namespace enclave::shard

#endif  // COMPONENTS_ENCLAVE_SHARD_CONNECTOR_ADDRESS_H_
