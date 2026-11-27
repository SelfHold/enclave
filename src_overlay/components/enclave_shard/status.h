// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_ENCLAVE_SHARD_STATUS_H_
#define COMPONENTS_ENCLAVE_SHARD_STATUS_H_

#include <string>

namespace enclave::shard {

// gRPC status codes used by the Connector API (CONNECTOR.md § 9).
enum class Code : int {
  kOk = 0,
  kCancelled = 1,
  kUnknown = 2,
  kInvalidArgument = 3,
  kDeadlineExceeded = 4,
  kNotFound = 5,
  kPermissionDenied = 7,
  kResourceExhausted = 8,
  kFailedPrecondition = 9,
  kAborted = 10,
  kUnimplemented = 12,
  kInternal = 13,
  kUnavailable = 14,
  kUnauthenticated = 16,
};

// Result of one call. `transport` is true when Shard was not reached at
// all (no connector.json, connection refused, socket gone): the browser then
// stays standalone and looks again later (CONNECTOR.md § 9, last paragraph).
struct Status {
  Code code = Code::kOk;
  std::string message;
  bool transport = false;

  static Status Ok() { return {}; }
  static Status Transport(std::string message);
  static Status FromWire(int code, std::string message);

  bool ok() const { return code == Code::kOk; }
  std::string ToString() const;
};

}  // namespace enclave::shard

#endif  // COMPONENTS_ENCLAVE_SHARD_STATUS_H_
