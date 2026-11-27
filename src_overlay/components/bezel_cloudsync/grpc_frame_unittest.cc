// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/bezel_cloudsync/grpc_frame.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace bezel::cloudsync {
namespace {

using Result = GrpcMessageReader::Result;

TEST(GrpcFrameTest, EncodesPrefix) {
  EXPECT_EQ(EncodeGrpcMessage("abc"), std::string("\0\0\0\0\3abc", 8));
  EXPECT_EQ(EncodeGrpcMessage(""), std::string(5, '\0'));
}

TEST(GrpcFrameTest, ReadsMessagesSplitAnywhere) {
  const std::string stream =
      EncodeGrpcMessage("first") + EncodeGrpcMessage("") +
      EncodeGrpcMessage(std::string(70000, 'x'));
  GrpcMessageReader reader;
  std::vector<std::string> messages;
  std::string message;
  for (char c : stream) {
    reader.Append(std::string_view(&c, 1));
    while (reader.Next(&message) == Result::kMessage) {
      messages.push_back(message);
    }
  }
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[0], "first");
  EXPECT_EQ(messages[1], "");
  EXPECT_EQ(messages[2].size(), 70000u);
  EXPECT_FALSE(reader.has_partial());
}

TEST(GrpcFrameTest, PartialMessageIsKept) {
  GrpcMessageReader reader;
  reader.Append(EncodeGrpcMessage("hello").substr(0, 7));
  std::string message;
  EXPECT_EQ(reader.Next(&message), Result::kNeedMore);
  EXPECT_TRUE(reader.has_partial());
}

TEST(GrpcFrameTest, RejectsCompressedAndHuge) {
  GrpcMessageReader compressed;
  compressed.Append(std::string("\1\0\0\0\1x", 6));
  std::string message;
  EXPECT_EQ(compressed.Next(&message), Result::kError);

  GrpcMessageReader huge;
  huge.Append(std::string("\0\x7f\xff\xff\xff", 5));
  EXPECT_EQ(huge.Next(&message), Result::kError);
}

TEST(GrpcFrameTest, PercentCoding) {
  EXPECT_EQ(PercentDecodeGrpcMessage("vault%20is%20locked"),
            "vault is locked");
  EXPECT_EQ(PercentDecodeGrpcMessage("100%"), "100%");
  EXPECT_EQ(PercentDecodeGrpcMessage("%zz"), "%zz");
  const std::string text = "Ключ: 50% \n";
  EXPECT_EQ(PercentDecodeGrpcMessage(PercentEncodeGrpcMessage(text)), text);
  EXPECT_EQ(PercentEncodeGrpcMessage("a%b"), "a%25b");
}

TEST(GrpcFrameTest, ParsesStatus) {
  EXPECT_EQ(ParseGrpcStatus("0"), 0);
  EXPECT_EQ(ParseGrpcStatus("16"), 16);
  EXPECT_EQ(ParseGrpcStatus(""), std::nullopt);
  EXPECT_EQ(ParseGrpcStatus("x"), std::nullopt);
  EXPECT_EQ(ParseGrpcStatus("-1"), std::nullopt);
}

}  // namespace
}  // namespace bezel::cloudsync
