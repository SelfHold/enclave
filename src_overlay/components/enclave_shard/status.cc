// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/status.h"

#include "base/strings/string_number_conversions.h"

namespace enclave::shard {

// static
Status Status::Transport(std::string message) {
  return {Code::kUnavailable, std::move(message), /*transport=*/true};
}

// static
Status Status::FromWire(int code, std::string message) {
  // Codes this client does not know are kept as they are: callers compare
  // against the ones they handle and treat the rest as errors.
  return {static_cast<Code>(code), std::move(message), /*transport=*/false};
}

std::string Status::ToString() const {
  std::string text = base::NumberToString(static_cast<int>(code));
  if (transport) {
    text += " (transport)";
  }
  if (!message.empty()) {
    text += ": " + message;
  }
  return text;
}

}  // namespace enclave::shard
