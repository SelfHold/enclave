// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/grpc_frame.h"

#include <cstdint>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

namespace enclave::shard {

std::string EncodeGrpcMessage(std::string_view message) {
  const uint32_t size = static_cast<uint32_t>(message.size());
  std::string out;
  out.reserve(kGrpcPrefixSize + message.size());
  out.push_back('\0');
  out.push_back(static_cast<char>((size >> 24) & 0xff));
  out.push_back(static_cast<char>((size >> 16) & 0xff));
  out.push_back(static_cast<char>((size >> 8) & 0xff));
  out.push_back(static_cast<char>(size & 0xff));
  out.append(message);
  return out;
}

void GrpcMessageReader::Append(std::string_view bytes) {
  // Drop what was already handed out before growing the buffer.
  if (offset_ > 0) {
    buffer_.erase(0, offset_);
    offset_ = 0;
  }
  buffer_.append(bytes);
}

GrpcMessageReader::Result GrpcMessageReader::Next(std::string* message) {
  const size_t available = buffer_.size() - offset_;
  if (available < kGrpcPrefixSize) {
    return Result::kNeedMore;
  }
  auto byte = [&](size_t i) {
    return static_cast<size_t>(static_cast<uint8_t>(buffer_[offset_ + i]));
  };
  if (byte(0) != 0) {
    return Result::kError;  // Compressed: we never ask for it.
  }
  const size_t size =
      (byte(1) << 24) | (byte(2) << 16) | (byte(3) << 8) | byte(4);
  if (size > kMaxGrpcMessageSize) {
    return Result::kError;
  }
  if (available < kGrpcPrefixSize + size) {
    return Result::kNeedMore;
  }
  message->assign(buffer_, offset_ + kGrpcPrefixSize, size);
  offset_ += kGrpcPrefixSize + size;
  return Result::kMessage;
}

std::string PercentDecodeGrpcMessage(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() &&
        base::IsHexDigit(value[i + 1]) && base::IsHexDigit(value[i + 2])) {
      out.push_back(static_cast<char>((base::HexDigitToInt(value[i + 1]) << 4) |
                                      base::HexDigitToInt(value[i + 2])));
      i += 2;
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

std::string PercentEncodeGrpcMessage(std::string_view value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (char c : value) {
    const auto byte = static_cast<uint8_t>(c);
    if (byte >= 0x20 && byte <= 0x7e && byte != '%') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0xf]);
    }
  }
  return out;
}

std::optional<int> ParseGrpcStatus(std::string_view value) {
  int code = 0;
  if (value.empty() || !base::StringToInt(value, &code) || code < 0) {
    return std::nullopt;
  }
  return code;
}

}  // namespace enclave::shard
