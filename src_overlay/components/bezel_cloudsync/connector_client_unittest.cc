// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/bezel_cloudsync/connector_client.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "components/bezel_cloudsync/fake_connector_server.h"
#include "components/bezel_cloudsync/grpc_transport.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace bezel::cloudsync {
namespace {

template <typename Reply>
using Future = base::test::TestFuture<Status, Reply>;

// Echoes Put's data back as an Item, to test both directions.
Status EchoPut(const std::string& request, std::string* reply) {
  pb::PutRequest put;
  if (!put.ParseFromString(request)) {
    return {Code::kInvalidArgument, "bad request"};
  }
  pb::Item item;
  item.set_key(put.key());
  item.set_revision("r2");
  item.add_versions()->set_data(put.data());
  *reply = item.SerializeAsString();
  return Status::Ok();
}

class RecordingWatcher : public ConnectorClient::WatchObserver {
 public:
  void OnChange(const pb::Change& change) override {
    changes.push_back(change);
    if (quit_on_change) {
      std::move(quit_on_change).Run();
    }
  }
  void OnWatchEnd(Status status) override {
    end = status;
    if (quit_on_end) {
      std::move(quit_on_end).Run();
    }
  }

  std::vector<pb::Change> changes;
  std::optional<Status> end;
  base::OnceClosure quit_on_change;
  base::OnceClosure quit_on_end;
};

class ConnectorClientTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    ASSERT_TRUE(
        server_.Start(dir_.GetPath().AppendASCII("connector.sock").value()));
    address_ = server_.address();
    auto transport = std::make_unique<GrpcTransport>(base::BindRepeating(
        [](std::optional<ConnectorAddress>* address) { return *address; },
        base::Unretained(&address_)));
    transport_ = transport.get();
    client_ = std::make_unique<ConnectorClient>(std::move(transport));
  }

  pb::HelloReply HelloReady() {
    Future<pb::HelloReply> hello;
    pb::HelloRequest request;
    request.set_app_id(kBezelAppId);
    client_->Hello(request, hello.GetCallback());
    EXPECT_TRUE(hello.Get<0>().ok()) << hello.Get<0>().ToString();
    return hello.Get<1>();
  }

  void ServeHello() {
    server_.SetHandler("Hello", base::BindRepeating(
                                    [](const std::string& request,
                                       std::string* reply) {
                                      pb::HelloRequest hello;
                                      hello.ParseFromString(request);
                                      pb::HelloReply answer;
                                      answer.set_api_version(1);
                                      answer.set_state(pb::READY);
                                      answer.set_app_id(hello.app_id());
                                      answer.set_vault_id("vault-1");
                                      *reply = answer.SerializeAsString();
                                      return Status::Ok();
                                    }));
  }

  base::test::TaskEnvironment env_{
      base::test::TaskEnvironment::MainThreadType::IO};
  base::ScopedTempDir dir_;
  FakeConnectorServer server_;
  std::optional<ConnectorAddress> address_;
  raw_ptr<GrpcTransport> transport_ = nullptr;
  std::unique_ptr<ConnectorClient> client_;
};

TEST_F(ConnectorClientTest, HelloRoundTrip) {
  ServeHello();
  const pb::HelloReply reply = HelloReady();
  EXPECT_EQ(reply.state(), pb::READY);
  EXPECT_EQ(reply.app_id(), "enclave");
  EXPECT_EQ(reply.vault_id(), "vault-1");
  ASSERT_EQ(server_.requests().size(), 1u);
  EXPECT_EQ(server_.requests()[0].method, "Hello");
  EXPECT_EQ(server_.requests()[0].authorization, "");
}

TEST_F(ConnectorClientTest, TokenGoesWithPairedCallsOnly) {
  ServeHello();
  server_.SetHandler("RequestPairing",
                     base::BindRepeating([](const std::string&, std::string* r) {
                       pb::PairingTicket ticket;
                       ticket.set_ticket("t");
                       ticket.set_code("123456");
                       *r = ticket.SerializeAsString();
                       return Status::Ok();
                     }));
  client_->SetToken("secret-token");
  HelloReady();

  Future<pb::PairingTicket> ticket;
  pb::PairingRequest pairing;
  pairing.set_app_id(kBezelAppId);
  pairing.add_scopes("passwords");
  client_->RequestPairing(pairing, ticket.GetCallback());
  ASSERT_TRUE(ticket.Get<0>().ok());
  EXPECT_EQ(ticket.Get<1>().code(), "123456");

  ASSERT_EQ(server_.requests().size(), 2u);
  EXPECT_EQ(server_.requests()[0].authorization, "Bearer secret-token");
  EXPECT_EQ(server_.requests()[1].authorization, "");
}

TEST_F(ConnectorClientTest, ServerErrorsKeepCodeAndMessage) {
  server_.SetHandler("Put",
                     base::BindRepeating([](const std::string&, std::string*) {
                       return Status{Code::kAborted, "base is not current: r7"};
                     }));
  Future<pb::Item> put;
  client_->Put(pb::PutRequest(), put.GetCallback());
  EXPECT_EQ(put.Get<0>().code, Code::kAborted);
  EXPECT_EQ(put.Get<0>().message, "base is not current: r7");
  EXPECT_FALSE(put.Get<0>().transport);

  Future<pb::AppKeyReply> key;  // No handler.
  client_->AppKey(key.GetCallback());
  EXPECT_EQ(key.Get<0>().code, Code::kUnimplemented);
}

TEST_F(ConnectorClientTest, LargeRecordBothWays) {
  // 256 KiB is the record limit: larger than HTTP/2's initial 64 KiB window,
  // so this needs flow control on both sides.
  server_.SetHandler("Put", base::BindRepeating(&EchoPut));
  pb::PutRequest put;
  put.set_key("t/bookmarks/0");
  put.set_data(std::string(256 * 1024, 'b'));
  for (int i = 0; i < 3; ++i) {
    Future<pb::Item> item;
    client_->Put(put, item.GetCallback());
    ASSERT_TRUE(item.Get<0>().ok()) << item.Get<0>().ToString();
    ASSERT_EQ(item.Get<1>().versions_size(), 1);
    EXPECT_EQ(item.Get<1>().versions(0).data(), put.data());
  }
}

TEST_F(ConnectorClientTest, ConcurrentCalls) {
  server_.SetHandler("Put", base::BindRepeating(&EchoPut));
  std::vector<std::unique_ptr<Future<pb::Item>>> futures;
  for (int i = 0; i < 20; ++i) {
    pb::PutRequest put;
    put.set_key("k" + base::NumberToString(i));
    put.set_data(std::string(10000 + i, 'x'));
    futures.push_back(std::make_unique<Future<pb::Item>>());
    client_->Put(put, futures.back()->GetCallback());
  }
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(futures[i]->Get<0>().ok());
    EXPECT_EQ(futures[i]->Get<1>().key(), "k" + base::NumberToString(i));
    EXPECT_EQ(futures[i]->Get<1>().versions(0).data().size(),
              static_cast<size_t>(10000 + i));
  }
}

TEST_F(ConnectorClientTest, NotRunningIsATransportError) {
  address_.reset();
  Future<pb::HelloReply> hello;
  client_->Hello(pb::HelloRequest(), hello.GetCallback());
  EXPECT_TRUE(hello.Get<0>().transport);
  EXPECT_EQ(hello.Get<0>().code, Code::kUnavailable);
}

TEST_F(ConnectorClientTest, StaleConnectorJsonIsATransportError) {
  address_->unix_path = dir_.GetPath().AppendASCII("gone.sock").value();
  Future<pb::HelloReply> hello;
  client_->Hello(pb::HelloRequest(), hello.GetCallback());
  EXPECT_TRUE(hello.Get<0>().transport);
}

TEST_F(ConnectorClientTest, NewerApiIsRefused) {
  address_->api_version = 2;
  Future<pb::HelloReply> hello;
  client_->Hello(pb::HelloRequest(), hello.GetCallback());
  EXPECT_EQ(hello.Get<0>().code, Code::kUnimplemented);
  EXPECT_TRUE(server_.requests().empty());
}

TEST_F(ConnectorClientTest, Timeout) {
  server_.Hold("Get");
  base::test::TestFuture<Status, std::string> reply;
  transport_->Call("Get", "", base::Milliseconds(200), reply.GetCallback());
  EXPECT_EQ(reply.Get<0>().code, Code::kDeadlineExceeded);

  // The connection is still usable.
  ServeHello();
  HelloReady();
}

TEST_F(ConnectorClientTest, WatchDeliversChangesAndEnds) {
  ServeHello();
  RecordingWatcher watcher;
  auto handle = client_->Watch(&watcher);
  HelloReady();  // The watch is open once a later call came back.
  ASSERT_EQ(server_.open_watches(), 1u);

  pb::Change change;
  change.set_kind(pb::Change::RECORD);
  change.set_key("t/bookmarks/3");
  change.set_revision("r9");
  {
    base::RunLoop loop;
    watcher.quit_on_change = loop.QuitClosure();
    server_.PushChange(change.SerializeAsString());
    loop.Run();
  }
  ASSERT_EQ(watcher.changes.size(), 1u);
  EXPECT_EQ(watcher.changes[0].key(), "t/bookmarks/3");
  EXPECT_EQ(watcher.changes[0].revision(), "r9");

  {
    base::RunLoop loop;
    watcher.quit_on_end = loop.QuitClosure();
    server_.EndWatches({Code::kUnauthenticated, "unpaired"});
    loop.Run();
  }
  ASSERT_TRUE(watcher.end);
  EXPECT_EQ(watcher.end->code, Code::kUnauthenticated);
}

TEST_F(ConnectorClientTest, DroppingTheHandleStopsTheWatch) {
  ServeHello();
  RecordingWatcher watcher;
  auto handle = client_->Watch(&watcher);
  HelloReady();
  ASSERT_EQ(server_.open_watches(), 1u);
  handle.reset();
  HelloReady();  // The reset reached the server before this answer.
  EXPECT_EQ(server_.open_watches(), 0u);
  EXPECT_FALSE(watcher.end);
}

TEST_F(ConnectorClientTest, DaemonStopEndsWatchAndNextCallReconnects) {
  ServeHello();
  RecordingWatcher watcher;
  auto handle = client_->Watch(&watcher);
  HelloReady();
  {
    base::RunLoop loop;
    watcher.quit_on_end = loop.QuitClosure();
    server_.DropConnections();
    loop.Run();
  }
  ASSERT_TRUE(watcher.end);
  EXPECT_TRUE(watcher.end->transport);

  HelloReady();
  EXPECT_EQ(server_.requests().back().method, "Hello");
}

TEST_F(ConnectorClientTest, LoginRoundTrip) {
  server_.SetHandler(
      "PutLogin", base::BindRepeating([](const std::string& request,
                                         std::string* reply) {
        pb::PutLoginRequest put;
        put.ParseFromString(request);
        pb::Login login = put.login();
        login.set_id("L1");
        login.set_revision("r1");
        *reply = login.SerializeAsString();
        return Status::Ok();
      }));
  pb::PutLoginRequest put;
  put.mutable_login()->set_signon_realm("https://example.com/");
  put.mutable_login()->set_username("me");
  put.mutable_login()->set_password("пароль");
  (*put.mutable_login()->mutable_extra())["bezel.scheme"] = "0";
  Future<pb::Login> login;
  client_->PutLogin(put, login.GetCallback());
  ASSERT_TRUE(login.Get<0>().ok());
  EXPECT_EQ(login.Get<1>().id(), "L1");
  EXPECT_EQ(login.Get<1>().password(), "пароль");
  EXPECT_EQ(login.Get<1>().extra().at("bezel.scheme"), "0");
}

}  // namespace
}  // namespace bezel::cloudsync

namespace bezel::cloudsync {
namespace {

// Against a real cloudsyncd: set BEZEL_CLOUDSYNC_CONNECTOR_JSON to its
// connector.json and run with --gtest_also_run_disabled_tests. Checks what
// the fake cannot: HPACK, SETTINGS and framing of Go's gRPC server.
TEST(ConnectorInteropTest, DISABLED_RealDaemon) {
  base::test::TaskEnvironment env{
      base::test::TaskEnvironment::MainThreadType::IO};
  std::unique_ptr<base::Environment> vars = base::Environment::Create();
  const std::optional<std::string> path =
      vars->GetVar("BEZEL_CLOUDSYNC_CONNECTOR_JSON");
  ASSERT_TRUE(path) << "BEZEL_CLOUDSYNC_CONNECTOR_JSON is not set";
  ConnectorClient client(
      std::make_unique<GrpcTransport>(base::FilePath(*path)));

  Future<pb::HelloReply> hello;
  pb::HelloRequest request;
  request.set_app_id(kBezelAppId);
  client.Hello(request, hello.GetCallback());
  ASSERT_TRUE(hello.Get<0>().ok()) << hello.Get<0>().ToString();
  EXPECT_EQ(hello.Get<1>().api_version(), 1u);
  EXPECT_FALSE(hello.Get<1>().paired());
  LOG(INFO) << "state " << hello.Get<1>().state() << ", core "
            << hello.Get<1>().core_version();

  // Without a token: an error with a gRPC status, not a transport error.
  Future<pb::Item> get;
  pb::GetRequest get_request;
  get_request.set_key("meta");
  client.Get(get_request, get.GetCallback());
  EXPECT_FALSE(get.Get<0>().transport) << get.Get<0>().ToString();
  EXPECT_FALSE(get.Get<0>().ok());
  LOG(INFO) << "Get without pairing: " << get.Get<0>().ToString();

  // A watch without a token ends with a status as well.
  RecordingWatcher watcher;
  base::RunLoop loop;
  watcher.quit_on_end = loop.QuitClosure();
  auto handle = client.Watch(&watcher);
  loop.Run();
  ASSERT_TRUE(watcher.end);
  EXPECT_FALSE(watcher.end->transport) << watcher.end->ToString();
  LOG(INFO) << "Watch without pairing: " << watcher.end->ToString();
}

}  // namespace
}  // namespace bezel::cloudsync
