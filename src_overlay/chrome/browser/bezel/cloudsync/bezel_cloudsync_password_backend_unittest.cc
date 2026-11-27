// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "chrome/browser/bezel/cloudsync/bezel_cloudsync_password_backend.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "chrome/browser/bezel/cloudsync/bezel_password_conversion.h"
#include "components/bezel_cloudsync/connector_client.h"
#include "components/bezel_cloudsync/connector_transport.h"
#include "components/password_manager/core/browser/password_store/password_store_backend_error.h"
#include "components/password_manager/core/browser/password_store/password_store_change.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace bezel {
namespace {

namespace pb = cloudsync::pb;
namespace ps = password_sync;
using password_manager::PasswordStoreBackendError;
using password_manager::PasswordStoreChange;
using password_manager::PasswordStoreChangeList;
using password_manager::StoredCredential;

using LoginsOrError =
    base::expected<std::vector<StoredCredential>, PasswordStoreBackendError>;
using ChangesOrError = base::expected<std::optional<PasswordStoreChangeList>,
                                      PasswordStoreBackendError>;

// CloudSync's password store in memory (CONNECTOR.md § 12): ids, revisions,
// CAS on base, and a change journal for ListChanges.
class FakeLoginStore {
 public:
  std::string Add(pb::Login login) {
    login.set_id("L" + base::NumberToString(next_id_++));
    return Store(std::move(login), /*deleted=*/false);
  }

  void Remove(const std::string& id) {
    pb::Login login;
    login.set_id(id);
    logins_.erase(id);
    Journal(id, "", /*deleted=*/true);
  }

  void ChangeElsewhere(const std::string& id, const std::string& password) {
    pb::Login login = logins_.at(id);
    login.set_password(password);
    Store(std::move(login), false);
  }

  const pb::Login* Find(const std::string& id) const {
    auto it = logins_.find(id);
    return it == logins_.end() ? nullptr : &it->second;
  }
  size_t size() const { return logins_.size(); }

  // Calls by method.
  std::map<std::string, int> calls;
  // The next PutLogin fails with ABORTED once (another device wrote first).
  bool conflict_next_put = false;

  void Handle(std::string_view method,
              const std::string& request,
              cloudsync::ConnectorTransport::UnaryCallback callback) {
    ++calls[std::string(method)];
    cloudsync::Status status;
    std::string reply;
    if (method == "ListLogins") {
      pb::ListLoginsRequest req;
      req.ParseFromString(request);
      // Pages of 2, to exercise paging.
      pb::ListLoginsReply out;
      size_t start = req.page_token().empty()
                         ? 0
                         : static_cast<size_t>(std::stoul(req.page_token()));
      size_t i = 0;
      for (const auto& [id, login] : logins_) {
        if (i >= start && i < start + 2) {
          *out.add_logins() = login;
        }
        ++i;
      }
      if (start + 2 < logins_.size()) {
        out.set_next_page_token(base::NumberToString(start + 2));
      }
      out.set_cursor(base::NumberToString(journal_.size()));
      reply = out.SerializeAsString();
    } else if (method == "GetLogins") {
      pb::GetLoginsRequest req;
      req.ParseFromString(request);
      pb::ListLoginsReply out;
      for (const std::string& id : req.ids()) {
        if (const pb::Login* login = Find(id)) {
          *out.add_logins() = *login;
        }
      }
      reply = out.SerializeAsString();
    } else if (method == "PutLogin") {
      pb::PutLoginRequest req;
      req.ParseFromString(request);
      pb::Login login = req.login();
      const pb::Login* current =
          login.id().empty() ? nullptr : Find(login.id());
      if (conflict_next_put) {
        conflict_next_put = false;
        if (current) {
          ChangeElsewhere(login.id(), current->password());
        }
        status = cloudsync::Status::FromWire(10, "conflict");
      } else if (!login.id().empty() && !current) {
        status = cloudsync::Status::FromWire(5, "gone");
      } else if (current && current->revision() != req.base()) {
        status = cloudsync::Status::FromWire(10, "conflict");
      } else {
        std::string id = login.id().empty() ? Add(login) : Store(login, false);
        reply = logins_.at(id).SerializeAsString();
      }
    } else if (method == "DeleteLogin") {
      pb::DeleteLoginRequest req;
      req.ParseFromString(request);
      const pb::Login* current = Find(req.id());
      if (!current) {
        status = cloudsync::Status::FromWire(5, "gone");
      } else if (current->revision() != req.base()) {
        status = cloudsync::Status::FromWire(10, "conflict");
      } else {
        Remove(req.id());
        reply = pb::DeleteLoginReply().SerializeAsString();
      }
    } else if (method == "TouchLogin") {
      reply = pb::TouchLoginReply().SerializeAsString();
    } else if (method == "ListChanges") {
      pb::ListChangesRequest req;
      req.ParseFromString(request);
      pb::ListChangesReply out;
      size_t from = 0;
      base::StringToSizeT(req.cursor(), &from);
      for (size_t i = from; i < journal_.size(); ++i) {
        *out.add_changes() = journal_[i];
      }
      out.set_cursor(base::NumberToString(journal_.size()));
      reply = out.SerializeAsString();
    } else {
      status = cloudsync::Status::FromWire(12, "unimplemented");
    }
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), std::move(status),
                                  std::move(reply)));
  }

 private:
  std::string Store(pb::Login login, bool deleted) {
    login.set_revision("r" + base::NumberToString(next_revision_++));
    std::string id = login.id();
    Journal(id, login.revision(), deleted);
    logins_[id] = std::move(login);
    return id;
  }

  void Journal(const std::string& id,
               const std::string& revision,
               bool deleted) {
    pb::LoginChange change;
    change.set_id(id);
    change.set_revision(revision);
    change.set_deleted(deleted);
    journal_.push_back(std::move(change));
  }

  std::map<std::string, pb::Login> logins_;
  std::vector<pb::LoginChange> journal_;
  int next_id_ = 1;
  int next_revision_ = 1;
};

class FakeTransport : public cloudsync::ConnectorTransport {
 public:
  explicit FakeTransport(FakeLoginStore* store) : store_(store) {}

  void Call(std::string_view method,
            std::string request,
            base::TimeDelta timeout,
            UnaryCallback callback) override {
    store_->Handle(method, request, std::move(callback));
  }
  std::unique_ptr<WatchHandle> Watch(WatchObserver* observer) override {
    return nullptr;
  }
  void SetToken(std::string token) override {}

 private:
  raw_ptr<FakeLoginStore> store_;
};

StoredCredential Cred(const std::string& site,
                      const std::string& user,
                      const std::string& password) {
  StoredCredential cred;
  cred.signon_realm = "https://" + site + "/";
  cred.url = GURL("https://" + site + "/login");
  cred.username_value = base::UTF8ToUTF16(user);
  cred.password_value =
      password_manager::PasswordString(base::UTF8ToUTF16(password));
  cred.date_created = base::Time::Now();
  cred.date_password_modified = base::Time::Now();
  return cred;
}

pb::Login Login(const std::string& site,
                const std::string& user,
                const std::string& password,
                const std::string& tag) {
  return ps::LoginFromCredential(Cred(site, user, password), tag);
}

std::vector<std::string> Users(const std::vector<StoredCredential>& creds) {
  std::vector<std::string> out;
  for (const StoredCredential& cred : creds) {
    out.push_back(base::UTF16ToUTF8(cred.username_value));
  }
  std::sort(out.begin(), out.end());
  return out;
}

class CloudSyncPasswordBackendTest : public testing::Test {
 protected:
  void SetUp() override {
    client_ = std::make_unique<cloudsync::ConnectorClient>(
        std::make_unique<FakeTransport>(&store_));
  }

  void TearDown() override {
    if (backend_) {
      base::RunLoop run_loop;
      backend_->Shutdown(run_loop.QuitClosure());
      run_loop.Run();
    }
  }

  // Starts the backend for `slot` and waits for the first load.
  void Start(const std::string& slot, bool primary) {
    backend_ = std::make_unique<CloudSyncPasswordBackend>(base::FilePath());
    base::test::TestFuture<bool> init;
    backend_->InitBackend(
        base::BindRepeating(&CloudSyncPasswordBackendTest::OnRemote,
                            base::Unretained(this)),
        base::DoNothing(), init.GetCallback());
    ASSERT_TRUE(init.Get());
    backend_->SetConnection(client_.get(), slot, primary);
    task_environment_.RunUntilIdle();
    ASSERT_TRUE(backend_->loaded_for_testing());
  }

  std::vector<StoredCredential> All() {
    base::test::TestFuture<LoginsOrError> future;
    backend_->GetAllLoginsAsync(future.GetCallback());
    LoginsOrError result = future.Take();
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? std::move(*result)
                              : std::vector<StoredCredential>();
  }

  ChangesOrError Add(StoredCredential cred) {
    base::test::TestFuture<ChangesOrError> future;
    backend_->AddLoginAsync(std::move(cred), future.GetCallback());
    return future.Take();
  }

  ChangesOrError Update(StoredCredential cred) {
    base::test::TestFuture<ChangesOrError> future;
    backend_->UpdateLoginAsync(std::move(cred), future.GetCallback());
    return future.Take();
  }

  ChangesOrError Remove(StoredCredential cred) {
    base::test::TestFuture<ChangesOrError> future;
    backend_->RemoveLoginAsync(FROM_HERE, std::move(cred),
                               future.GetCallback());
    return future.Take();
  }

  void OnRemote(ChangesOrError changes) {
    remote_.push_back(std::move(changes));
  }

  base::test::TaskEnvironment task_environment_;
  FakeLoginStore store_;
  std::unique_ptr<cloudsync::ConnectorClient> client_;
  std::unique_ptr<CloudSyncPasswordBackend> backend_;
  std::vector<ChangesOrError> remote_;
};

TEST_F(CloudSyncPasswordBackendTest, PrimaryProfileSeesItsOwnAndUntagged) {
  store_.Add(Login("a.example", "mine", "1", "slotA"));
  store_.Add(Login("b.example", "untagged", "2", ""));
  store_.Add(Login("c.example", "other", "3", "slotB"));
  store_.Add(Login("d.example", "mine2", "4", "slotA"));
  Start("slotA", /*primary=*/true);
  EXPECT_EQ(Users(All()),
            (std::vector<std::string>{"mine", "mine2", "untagged"}));
  EXPECT_EQ(store_.calls["ListLogins"], 2);  // Two pages.
}

TEST_F(CloudSyncPasswordBackendTest, OtherProfileSeesOnlyItsOwn) {
  store_.Add(Login("a.example", "mine", "1", "slotA"));
  store_.Add(Login("b.example", "untagged", "2", ""));
  store_.Add(Login("c.example", "other", "3", "slotB"));
  Start("slotB", /*primary=*/false);
  EXPECT_EQ(Users(All()), (std::vector<std::string>{"other"}));
}

TEST_F(CloudSyncPasswordBackendTest, AddWritesTaggedLogin) {
  Start("slotA", true);
  ChangesOrError result = Add(Cred("new.example", "u", "secret"));
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->has_value());
  ASSERT_EQ((*result)->size(), 1u);
  EXPECT_EQ((*result)->front().type(), PasswordStoreChange::ADD);
  ASSERT_EQ(store_.size(), 1u);
  const pb::Login* login = store_.Find("L1");
  ASSERT_TRUE(login);
  EXPECT_EQ(login->password(), "secret");
  EXPECT_EQ(ps::ProfileTagOf(*login), "slotA");
  EXPECT_EQ(Users(All()), (std::vector<std::string>{"u"}));
}

TEST_F(CloudSyncPasswordBackendTest, AddOverSameKeyReplaces) {
  store_.Add(Login("a.example", "u", "old", "slotA"));
  Start("slotA", true);
  ChangesOrError result = Add(Cred("a.example", "u", "new"));
  ASSERT_TRUE(result.has_value() && result->has_value());
  ASSERT_EQ((*result)->size(), 2u);
  EXPECT_EQ((*result)->at(0).type(), PasswordStoreChange::REMOVE);
  EXPECT_EQ((*result)->at(1).type(), PasswordStoreChange::ADD);
  EXPECT_EQ(store_.size(), 1u);
  EXPECT_EQ(store_.Find("L1")->password(), "new");
}

TEST_F(CloudSyncPasswordBackendTest, UpdateRetriesAfterConflict) {
  store_.Add(Login("a.example", "u", "old", "slotA"));
  Start("slotA", true);
  store_.conflict_next_put = true;
  ChangesOrError result = Update(Cred("a.example", "u", "new"));
  ASSERT_TRUE(result.has_value() && result->has_value());
  ASSERT_EQ((*result)->size(), 1u);
  EXPECT_EQ((*result)->front().type(), PasswordStoreChange::UPDATE);
  EXPECT_EQ(store_.calls["PutLogin"], 2);
  EXPECT_EQ(store_.Find("L1")->password(), "new");
}

TEST_F(CloudSyncPasswordBackendTest, UpdateOfUnknownLoginChangesNothing) {
  Start("slotA", true);
  ChangesOrError result = Update(Cred("a.example", "u", "new"));
  ASSERT_TRUE(result.has_value() && result->has_value());
  EXPECT_TRUE((*result)->empty());
  EXPECT_EQ(store_.calls["PutLogin"], 0);
}

TEST_F(CloudSyncPasswordBackendTest, FillOnlyTouches) {
  store_.Add(Login("a.example", "u", "pw", "slotA"));
  Start("slotA", true);
  std::vector<StoredCredential> all = All();
  ASSERT_EQ(all.size(), 1u);
  StoredCredential used = std::move(all[0]);
  used.times_used_in_html_form += 1;
  used.date_last_used = base::Time::Now();
  ChangesOrError result = Update(std::move(used));
  ASSERT_TRUE(result.has_value() && result->has_value());
  EXPECT_EQ(store_.calls["PutLogin"], 0);
  EXPECT_EQ(store_.calls["TouchLogin"], 1);
}

TEST_F(CloudSyncPasswordBackendTest, RemoveDeletesInCloudSync) {
  store_.Add(Login("a.example", "u", "pw", "slotA"));
  Start("slotA", true);
  ChangesOrError result = Remove(Cred("a.example", "u", "pw"));
  ASSERT_TRUE(result.has_value() && result->has_value());
  ASSERT_EQ((*result)->size(), 1u);
  EXPECT_EQ((*result)->front().type(), PasswordStoreChange::REMOVE);
  EXPECT_EQ(store_.size(), 0u);
  EXPECT_TRUE(All().empty());
}

TEST_F(CloudSyncPasswordBackendTest, RemoteChangesReachThePasswordManager) {
  std::string kept = store_.Add(Login("a.example", "u", "pw", "slotA"));
  Start("slotA", true);
  remote_.clear();

  store_.Add(Login("b.example", "new", "x", "slotA"));
  store_.Add(Login("c.example", "foreign", "y", "slotB"));
  store_.ChangeElsewhere(kept, "changed");
  backend_->OnRemoteLoginsChanged();
  task_environment_.RunUntilIdle();

  ASSERT_EQ(remote_.size(), 1u);
  ASSERT_TRUE(remote_[0].has_value() && remote_[0]->has_value());
  const PasswordStoreChangeList& changes = **remote_[0];
  ASSERT_EQ(changes.size(), 2u);  // The foreign login is not news here.
  int adds = 0, updates = 0;
  for (const PasswordStoreChange& change : changes) {
    adds += change.type() == PasswordStoreChange::ADD;
    updates += change.type() == PasswordStoreChange::UPDATE;
  }
  EXPECT_EQ(adds, 1);
  EXPECT_EQ(updates, 1);

  store_.Remove(kept);
  backend_->OnRemoteLoginsChanged();
  task_environment_.RunUntilIdle();
  ASSERT_EQ(remote_.size(), 2u);
  ASSERT_TRUE(remote_[1].has_value() && remote_[1]->has_value());
  ASSERT_EQ((*remote_[1])->size(), 1u);
  EXPECT_EQ((*remote_[1])->front().type(), PasswordStoreChange::REMOVE);
  EXPECT_EQ(Users(All()), (std::vector<std::string>{"new"}));
}

TEST_F(CloudSyncPasswordBackendTest, OwnWritesAreNotReportedTwice) {
  Start("slotA", true);
  ASSERT_TRUE(Add(Cred("a.example", "u", "pw")).has_value());
  remote_.clear();
  backend_->OnRemoteLoginsChanged();  // Watch echoes our own PutLogin.
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(remote_.empty());
  EXPECT_EQ(store_.calls["GetLogins"], 0);
}

TEST_F(CloudSyncPasswordBackendTest, WithoutConnectionEmptyAndReadOnly) {
  store_.Add(Login("a.example", "u", "pw", "slotA"));
  Start("slotA", true);
  ASSERT_EQ(All().size(), 1u);
  remote_.clear();

  backend_->SetConnection(nullptr, "", false);  // CloudSync locked.
  EXPECT_TRUE(All().empty());
  ASSERT_EQ(remote_.size(), 1u);  // The store reloads: everything is gone.
  ASSERT_TRUE(remote_[0].has_value());
  EXPECT_FALSE(remote_[0]->has_value());
  EXPECT_FALSE(Add(Cred("b.example", "u", "pw")).has_value());
  EXPECT_EQ(store_.size(), 1u);
}

TEST_F(CloudSyncPasswordBackendTest, ReadsWaitForTheFirstLoad) {
  store_.Add(Login("a.example", "u", "pw", "slotA"));
  backend_ = std::make_unique<CloudSyncPasswordBackend>(base::FilePath());
  base::test::TestFuture<bool> init;
  backend_->InitBackend(base::DoNothing(), base::DoNothing(),
                        init.GetCallback());
  ASSERT_TRUE(init.Get());
  base::test::TestFuture<LoginsOrError> early;
  backend_->SetConnection(client_.get(), "slotA", true);
  backend_->GetAllLoginsAsync(early.GetCallback());  // Before the reply.
  LoginsOrError result = early.Take();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1u);
}

TEST_F(CloudSyncPasswordBackendTest, FillMatchingFindsExactAndPsl) {
  store_.Add(Login("example.com", "exact", "1", "slotA"));
  store_.Add(Login("m.example.com", "psl", "2", "slotA"));
  store_.Add(Login("other.org", "none", "3", "slotA"));
  Start("slotA", true);
  password_manager::PasswordFormDigest digest(
      password_manager::PasswordForm::Scheme::kHtml, "https://example.com/",
      GURL("https://example.com/login"));

  base::test::TestFuture<LoginsOrError> exact;
  backend_->FillMatchingLoginsAsync(exact.GetCallback(), false, {digest});
  EXPECT_EQ(Users(*exact.Take()), (std::vector<std::string>{"exact"}));

  base::test::TestFuture<LoginsOrError> grouped;
  backend_->GetGroupedMatchingLoginsAsync(digest, grouped.GetCallback());
  LoginsOrError all = grouped.Take();
  EXPECT_EQ(Users(*all), (std::vector<std::string>{"exact", "psl"}));
  for (const StoredCredential& cred : *all) {
    EXPECT_TRUE(cred.match_type.has_value());
  }
}

}  // namespace
}  // namespace bezel
