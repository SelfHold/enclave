// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/enclave_shard/connector_address.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace enclave::shard {
namespace {

using Kind = ConnectorAddress::Kind;

TEST(ConnectorAddressTest, ParsesUnixSocket) {
  auto address = ParseConnectorJson(
      R"({"address": "unix:///run/user/1000/selfhold-shard-connector.sock",
          "api_version": 1})");
  ASSERT_TRUE(address);
  EXPECT_EQ(address->kind, Kind::kUnix);
  EXPECT_EQ(address->unix_path, "/run/user/1000/selfhold-shard-connector.sock");
  EXPECT_EQ(address->api_version, 1);
}

TEST(ConnectorAddressTest, ParsesLoopbackTcp) {
  auto address =
      ParseConnectorJson(R"({"address": "127.0.0.1:41234", "api_version": 1})");
  ASSERT_TRUE(address);
  EXPECT_EQ(address->kind, Kind::kTcp);
  EXPECT_EQ(address->host, "127.0.0.1");
  EXPECT_EQ(address->port, 41234);

  auto v6 = ParseConnectorJson(R"({"address": "[::1]:5", "api_version": 1})");
  ASSERT_TRUE(v6);
  EXPECT_EQ(v6->host, "::1");
}

TEST(ConnectorAddressTest, KeepsOtherApiVersions) {
  // The transport refuses them; parsing still works so it can tell the user.
  auto address = ParseConnectorJson(
      R"({"address": "unix:///tmp/s", "api_version": 2, "extra": true})");
  ASSERT_TRUE(address);
  EXPECT_EQ(address->api_version, 2);
}

TEST(ConnectorAddressTest, RejectsAnythingElse) {
  for (const char* json : {
           "",
           "[]",
           R"({"address": "unix:///tmp/s"})",
           R"({"api_version": 1})",
           R"({"address": "unix://relative", "api_version": 1})",
           R"({"address": "unix://", "api_version": 1})",
           R"({"address": "10.0.0.5:80", "api_version": 1})",
           R"({"address": "example.com:80", "api_version": 1})",
           R"({"address": "127.0.0.1", "api_version": 1})",
           R"({"address": "127.0.0.1:0", "api_version": 1})",
           R"({"address": "127.0.0.1:70000", "api_version": 1})",
           R"({"address": "http://127.0.0.1:80", "api_version": 1})",
       }) {
    EXPECT_FALSE(ParseConnectorJson(json)) << json;
  }
}

TEST(ConnectorAddressTest, ReadsFile) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("connector.json");
  EXPECT_FALSE(ReadConnectorJson(path));  // Daemon not running.
  ASSERT_TRUE(base::WriteFile(
      path, R"({"address": "unix:///tmp/x.sock", "api_version": 1})"));
  auto address = ReadConnectorJson(path);
  ASSERT_TRUE(address);
  EXPECT_EQ(address->unix_path, "/tmp/x.sock");
}

TEST(ConnectorAddressTest, PathEndsInSelfHoldShardDir) {
  const base::FilePath path = GetConnectorJsonPath();
#if BUILDFLAG(IS_ANDROID)
  EXPECT_TRUE(path.empty());
#else
  ASSERT_FALSE(path.empty());
  EXPECT_EQ(path.BaseName().value(), FILE_PATH_LITERAL("connector.json"));
  EXPECT_EQ(path.DirName().BaseName().value(), FILE_PATH_LITERAL("shard"));
  EXPECT_EQ(path.DirName().DirName().BaseName().value(),
            FILE_PATH_LITERAL("selfhold"));
#endif
}

}  // namespace
}  // namespace enclave::shard
