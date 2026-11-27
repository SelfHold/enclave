// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#ifndef COMPONENTS_ENCLAVE_SHARD_GRPC_FRAME_H_
#define COMPONENTS_ENCLAVE_SHARD_GRPC_FRAME_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace enclave::shard {

// gRPC over HTTP/2 carries each message as a 5-byte prefix (compressed flag,
// big-endian length) followed by the message. We never compress.
inline constexpr size_t kGrpcPrefixSize = 5;

// Largest message we accept: a record (256 KiB) or a page of 500 logins with
// their `extra` (16 KiB each) fits well below this.
inline constexpr size_t kMaxGrpcMessageSize = 16 * 1024 * 1024;

std::string EncodeGrpcMessage(std::string_view message);

// Splits a byte stream into gRPC messages.
class GrpcMessageReader {
 public:
  enum class Result { kMessage, kNeedMore, kError };

  void Append(std::string_view bytes);
  // kMessage fills `message`. kError: compressed or too large; the stream is
  // unusable afterwards.
  Result Next(std::string* message);
  // True when bytes of an unfinished message are left.
  bool has_partial() const { return offset_ < buffer_.size(); }

 private:
  std::string buffer_;
  size_t offset_ = 0;
};

// `grpc-message` is percent-encoded (gRPC over HTTP/2 spec).
std::string PercentDecodeGrpcMessage(std::string_view value);
std::string PercentEncodeGrpcMessage(std::string_view value);

// `grpc-status` value, or nullopt if it is not a decimal number.
std::optional<int> ParseGrpcStatus(std::string_view value);

}  // namespace enclave::shard

#endif  // COMPONENTS_ENCLAVE_SHARD_GRPC_FRAME_H_
