// Copyright 2026 The Enclave Authors. Use of this source code is governed by the
// GNU General Public License v3 that can be found in the Enclave LICENSE file.

#include "components/bezel_cloudsync/replicator.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "components/bezel_cloudsync/cloudsync_controller.h"
#include "components/bezel_cloudsync/fake_record_store.h"
#include "components/bezel_cloudsync/loopback_bridge.h"
#include "components/bezel_cloudsync/shared_pairing.h"
#include "components/sync/base/data_type.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/google/compression_utils.h"

namespace bezel::cloudsync {
namespace {

constexpr char kBookmarkId[] = "32904_bm1";

sync_pb::LoopbackServerEntity Bookmark(const std::string& id,
                                       const std::string& title) {
  sync_pb::LoopbackServerEntity entity;
  entity.set_type(sync_pb::LoopbackServerEntity_Type_UNIQUE);
  entity.mutable_entity()->set_id_string(id);
  entity.mutable_entity()->set_name(title);
  entity.mutable_entity()->mutable_specifics()->mutable_bookmark()->set_url(
      "https://example.org/" + title);
  return entity;
}

sync_pb::LoopbackServerEntity Password(const std::string& id) {
  sync_pb::LoopbackServerEntity entity;
  entity.set_type(sync_pb::LoopbackServerEntity_Type_UNIQUE);
  entity.mutable_entity()->set_id_string(id);
  entity.mutable_entity()->mutable_specifics()->mutable_password();
  return entity;
}

sync_pb::LoopbackServerEntity Visit(const std::string& id, base::Time when) {
  sync_pb::LoopbackServerEntity entity;
  entity.set_type(sync_pb::LoopbackServerEntity_Type_UNIQUE);
  entity.mutable_entity()->set_id_string(id);
  entity.mutable_entity()
      ->mutable_specifics()
      ->mutable_history()
      ->set_visit_time_windows_epoch_micros(
          when.ToDeltaSinceWindowsEpoch().InMicroseconds());
  return entity;
}

// Stands in for the engine's LoopbackServer.
class FakeReplica : public LoopbackReplica {
 public:
  explicit FakeReplica(base::FilePath file) : file_(std::move(file)) {
    LoopbackBridge::Get().RegisterReplica(file_, weak_factory_.GetWeakPtr());
  }
  ~FakeReplica() override {
    LoopbackBridge::Get().UnregisterReplica(file_);
  }

  // As if the client committed it.
  void Commit(sync_pb::LoopbackServerEntity entity) {
    entities_[entity.entity().id_string()] = entity;
    LoopbackBridge::Get().NotifyLocalCommit(file_, {entity});
  }

  Entities ExportEntities() override {
    Entities result;
    for (const auto& [id, entity] : entities_) {
      result.push_back(entity);
    }
    return result;
  }
  std::vector<int> ImportEntities(Entities entities) override {
    std::vector<int> fields;
    for (auto& entity : entities) {
      ++imports_;
      entities_[entity.entity().id_string()] = entity;
      fields.push_back(32904);
    }
    return fields;
  }
  void ResetWithKeystoreKey(std::vector<uint8_t> key) override {
    entities_.clear();
    keys_ = {std::move(key)};
    ++resets_;
  }
  std::vector<std::vector<uint8_t>> GetKeystoreKeys() override {
    return keys_;
  }

  std::map<std::string, sync_pb::LoopbackServerEntity> entities_;
  std::vector<std::vector<uint8_t>> keys_;
  int resets_ = 0;
  int imports_ = 0;

 private:
  const base::FilePath file_;
  base::WeakPtrFactory<FakeReplica> weak_factory_{this};
};

// One device: its replica, client and replicator.
struct Device {
  Device(FakeRecordStore& store,
         const base::FilePath& dir,
         const std::string& name,
         const base::Clock* clock,
         const std::string& prefix = "p/s1/")
      : file(dir.AppendASCII(name).AppendASCII("profile.pb")),
        replica(std::make_unique<FakeReplica>(file)),
        client(std::make_unique<ConnectorClient>(store.CreateTransport(name))),
        replicator(std::make_unique<Replicator>(
            client.get(),
            file,
            name,
            prefix,
            base::BindRepeating(
                [](std::vector<syncer::DataTypeSet>* refreshes,
                   syncer::DataTypeSet types) { refreshes->push_back(types); },
                &refreshes),
            clock)) {}

  base::FilePath file;
  std::unique_ptr<FakeReplica> replica;
  std::unique_ptr<ConnectorClient> client;
  std::vector<syncer::DataTypeSet> refreshes;
  std::unique_ptr<Replicator> replicator;
};

class ReplicatorTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }

  std::unique_ptr<Device> MakeDevice(const std::string& name,
                                     const std::string& prefix = "p/s1/") {
    return std::make_unique<Device>(store_, dir_.GetPath(), name,
                                    env_.GetMockClock(), prefix);
  }

  std::vector<uint8_t> Key() {
    return CloudSyncController::KeystoreKeyFromAppKey(std::string(32, 'k'),
                                                      "s1");
  }

  // Entities in a stored record (key without the profile prefix).
  std::vector<std::string> IdsIn(const std::string& key,
                                 const std::string& prefix = "p/s1/") {
    std::vector<std::string> ids;
    auto it = store_.items().find(prefix + key);
    if (it == store_.items().end() || it->second.versions_size() != 1) {
      return ids;
    }
    std::string raw;
    records::Shard shard;
    EXPECT_TRUE(
        compression::GzipUncompress(it->second.versions(0).data(), &raw));
    EXPECT_TRUE(shard.ParseFromString(raw));
    for (const records::Entry& entry : shard.entries()) {
      ids.push_back(entry.entity().entity().id_string());
    }
    return ids;
  }

  void Settle(base::TimeDelta delay = Replicator::kEntityDelay) {
    env_.FastForwardBy(delay + base::Seconds(1));
    env_.RunUntilIdle();
  }

  base::test::TaskEnvironment env_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir dir_;
  FakeRecordStore store_;
};

TEST_F(ReplicatorTest, FirstDeviceResetsWithSharedKeyAndUploads) {
  auto a = MakeDevice("a");
  a->replica->entities_[kBookmarkId] = Bookmark(kBookmarkId, "old");
  a->replicator->Start(Key());
  env_.RunUntilIdle();

  // The server started again with the vault's key; the client then commits
  // its local data again.
  EXPECT_EQ(a->replica->resets_, 1);
  EXPECT_EQ(a->replica->keys_.back(), Key());
  a->replica->Commit(Bookmark(kBookmarkId, "mine"));
  env_.RunUntilIdle();
  EXPECT_TRUE(store_.items().empty()) << "written only after the delay";

  Settle();
  EXPECT_EQ(IdsIn("t/32904/0"), std::vector<std::string>{kBookmarkId});
}

TEST_F(ReplicatorTest, SecondDeviceGetsTheVaultsData) {
  auto a = MakeDevice("a");
  a->replicator->Start(Key());
  env_.RunUntilIdle();
  a->replica->Commit(Bookmark(kBookmarkId, "from-a"));
  Settle();

  auto b = MakeDevice("b");
  b->replica->keys_ = {Key()};  // Already on this vault: no reset.
  b->replicator->Start(Key());
  env_.RunUntilIdle();
  EXPECT_EQ(b->replica->resets_, 0);
  ASSERT_TRUE(b->replica->entities_.contains(kBookmarkId));
  EXPECT_EQ(b->replica->entities_[kBookmarkId].entity().name(), "from-a");
  ASSERT_FALSE(b->refreshes.empty());
  EXPECT_TRUE(b->refreshes.back().Has(syncer::BOOKMARKS));
}

TEST_F(ReplicatorTest, LiveChangeReachesTheOtherDevice) {
  auto a = MakeDevice("a");
  auto b = MakeDevice("b");
  a->replica->keys_ = b->replica->keys_ = {Key()};
  a->replicator->Start(Key());
  b->replicator->Start(Key());
  env_.RunUntilIdle();

  a->replica->Commit(Bookmark(kBookmarkId, "v1"));
  Settle();
  // FakeRecordStore delivers RECORD changes to transports with a Watch; the
  // replicator itself has none, so tell B the way the controller would.
  const pb::Item& item = store_.items().at("p/s1/t/32904/0");
  b->replicator->OnRecordChanged("p/s1/t/32904/0", item.revision(), false);
  env_.RunUntilIdle();
  ASSERT_TRUE(b->replica->entities_.contains(kBookmarkId));
  EXPECT_EQ(b->replica->entities_[kBookmarkId].entity().name(), "v1");

  // An unchanged revision is our own write: no extra read or import.
  const int imports = b->replica->imports_;
  b->replicator->OnRecordChanged("p/s1/t/32904/0", item.revision(), false);
  env_.RunUntilIdle();
  EXPECT_EQ(b->replica->imports_, imports);
}

TEST_F(ReplicatorTest, LastWriterWinsAndConflictsMerge) {
  auto a = MakeDevice("a");
  auto b = MakeDevice("b");
  a->replica->keys_ = b->replica->keys_ = {Key()};
  a->replicator->Start(Key());
  b->replicator->Start(Key());
  env_.RunUntilIdle();

  a->replica->Commit(Bookmark(kBookmarkId, "older"));
  env_.FastForwardBy(base::Seconds(5));
  b->replica->Commit(Bookmark(kBookmarkId, "newer"));
  b->replica->Commit(Bookmark("32904_bm2", "only-b"));
  // Both flush; the second Put of the record is ABORTED, re-read, merged.
  Settle();
  Settle();

  std::vector<std::string> ids = IdsIn("t/32904/0");
  EXPECT_EQ(ids, (std::vector<std::string>{kBookmarkId, "32904_bm2"}));
  // A took B's newer title.
  const pb::Item& item = store_.items().at("p/s1/t/32904/0");
  a->replicator->OnRecordChanged("p/s1/t/32904/0", item.revision(), false);
  env_.RunUntilIdle();
  EXPECT_EQ(a->replica->entities_[kBookmarkId].entity().name(), "newer");
}

TEST_F(ReplicatorTest, SeveralVersionsAreMergedAndRewritten) {
  auto a = MakeDevice("a");
  a->replica->keys_ = {Key()};
  a->replicator->Start(Key());
  env_.RunUntilIdle();
  a->replica->Commit(Bookmark(kBookmarkId, "a"));
  Settle();

  // Another device wrote the same record offline from the same base.
  records::Shard shard;
  records::Entry* entry = shard.add_entries();
  *entry->mutable_entity() = Bookmark("32904_offline", "offline");
  entry->set_changed_ms(1);
  entry->set_device("c");
  std::string packed;
  ASSERT_TRUE(compression::GzipCompress(
      base::span<const char>(shard.SerializeAsString()), &packed));
  store_.AddConcurrentVersion("p/s1/t/32904/0", packed, "c");
  const pb::Item& item = store_.items().at("p/s1/t/32904/0");
  ASSERT_EQ(item.versions_size(), 2);
  a->replicator->OnRecordChanged("p/s1/t/32904/0", item.revision(), false);
  Settle();

  EXPECT_EQ(store_.items().at("p/s1/t/32904/0").versions_size(), 1);
  EXPECT_EQ(IdsIn("t/32904/0"),
            (std::vector<std::string>{kBookmarkId, "32904_offline"}));
  EXPECT_TRUE(a->replica->entities_.contains("32904_offline"));
}

TEST_F(ReplicatorTest, PasswordsAreNeverUploaded) {
  auto a = MakeDevice("a");
  a->replica->keys_ = {Key()};
  a->replicator->Start(Key());
  env_.RunUntilIdle();
  // PASSWORDS field number is 45873.
  a->replica->Commit(Password("45873_secret"));
  Settle();
  EXPECT_TRUE(store_.items().empty());
}

TEST_F(ReplicatorTest, HistoryGoesByWeekAndOldWeeksStayLocal) {
  auto a = MakeDevice("a");
  a->replica->keys_ = {Key()};
  a->replicator->Start(Key());
  env_.RunUntilIdle();
  const base::Time now = env_.GetMockClock()->Now();
  // HISTORY field number is 963985.
  a->replica->Commit(Visit("963985_new", now));
  a->replica->Commit(Visit("963985_old", now - base::Days(7 * 20)));
  Settle(Replicator::kEntityDelay);
  EXPECT_TRUE(store_.items().empty()) << "history waits longer";
  Settle(Replicator::kHistoryDelay);

  const int64_t week = (now - base::Time::UnixEpoch()).InDays() / 7;
  const std::string key = "h/w" + base::NumberToString(week);
  EXPECT_EQ(IdsIn(key), std::vector<std::string>{"963985_new"});
  EXPECT_EQ(store_.items().size(), 1u);
}

TEST_F(ReplicatorTest, UnchangedContentIsNotWrittenAgain) {
  auto a = MakeDevice("a");
  a->replica->keys_ = {Key()};
  a->replicator->Start(Key());
  env_.RunUntilIdle();
  a->replica->Commit(Bookmark(kBookmarkId, "same"));
  Settle();
  const size_t puts = store_.put_count();
  // Restart: the startup reconcile finds nothing new.
  a->replicator->Stop();
  a->replicator->Start(Key());
  env_.RunUntilIdle();
  Settle();
  EXPECT_EQ(store_.put_count(), puts);
}

TEST_F(ReplicatorTest, OfflineWritesWaitForWritable) {
  auto a = MakeDevice("a");
  a->replica->keys_ = {Key()};
  a->replicator->Start(Key());
  env_.RunUntilIdle();
  store_.set_fail_puts(static_cast<int>(Code::kUnavailable));
  a->replica->Commit(Bookmark(kBookmarkId, "x"));
  Settle();
  EXPECT_TRUE(IdsIn("t/32904/0").empty());
  store_.set_fail_puts(0);
  a->replicator->OnWritable();
  env_.RunUntilIdle();
  EXPECT_EQ(IdsIn("t/32904/0"), std::vector<std::string>{kBookmarkId});
}

TEST_F(ReplicatorTest, ProfilesDoNotMix) {
  auto work = MakeDevice("a-work", "p/w/");
  auto personal = MakeDevice("a-personal", "p/p/");
  work->replica->keys_ = personal->replica->keys_ = {Key()};
  work->replicator->Start(Key());
  personal->replicator->Start(Key());
  env_.RunUntilIdle();
  work->replica->Commit(Bookmark(kBookmarkId, "work"));
  personal->replica->Commit(Bookmark("32904_bm2", "personal"));
  Settle();
  EXPECT_EQ(IdsIn("t/32904/0", "p/w/"), std::vector<std::string>{kBookmarkId});
  EXPECT_EQ(IdsIn("t/32904/0", "p/p/"),
            std::vector<std::string>{"32904_bm2"});

  // Another device's Work profile gets Work's data only.
  auto other = MakeDevice("b-work", "p/w/");
  other->replica->keys_ = {Key()};
  other->replicator->Start(Key());
  env_.RunUntilIdle();
  EXPECT_TRUE(other->replica->entities_.contains(kBookmarkId));
  EXPECT_FALSE(other->replica->entities_.contains("32904_bm2"));
  // Records of other profiles count against the shared limit.
  EXPECT_EQ(other->replicator->stats().other_records, 1u);

  // A change to Personal's record is ignored by Work.
  const int imports = other->replica->imports_;
  other->replicator->OnRecordChanged(
      "p/p/t/32904/0", store_.items().at("p/p/t/32904/0").revision(), false);
  env_.RunUntilIdle();
  EXPECT_EQ(other->replica->imports_, imports);
}

// Regression: approving the pairing must leave sync on. A Hello sent before
// the token was stored answered "not paired" and switched sync off again.
TEST_F(ReplicatorTest, PairingKeepsSyncOn) {
  store_.set_required_token("tok");
  SharedPairing pairing(dir_.GetPath().AppendASCII("pairing.json"));
  CloudSyncController controller(
      store_.CreateTransport("a"), &pairing,
      dir_.GetPath().AppendASCII("work").AppendASCII("profile.pb"), "Default",
      "Work", "Enclave test", base::DoNothing());
  controller.Start();
  env_.RunUntilIdle();
  ASSERT_EQ(controller.state().phase, CloudSyncController::Phase::kReady);
  EXPECT_FALSE(controller.state().paired);

  controller.EnableSync();
  env_.RunUntilIdle();
  EXPECT_TRUE(controller.state().enabled);
  EXPECT_TRUE(controller.state().paired);
  EXPECT_TRUE(pairing.data().paired);
  EXPECT_EQ(pairing.data().token, "tok");
  EXPECT_EQ(controller.state().message, "");
  // The profile got a slot named after it.
  EXPECT_EQ(controller.state().slot_name, "Work");
  EXPECT_TRUE(controller.state().primary_slot);
  controller.Shutdown();
}

TEST(CloudSyncControllerTest, KeystoreKeyIsStablePerAppKeyAndSlot) {
  const auto one = CloudSyncController::KeystoreKeyFromAppKey("a", "s1");
  EXPECT_EQ(one.size(), 16u);
  EXPECT_EQ(one, CloudSyncController::KeystoreKeyFromAppKey("a", "s1"));
  EXPECT_NE(one, CloudSyncController::KeystoreKeyFromAppKey("b", "s1"));
  EXPECT_NE(one, CloudSyncController::KeystoreKeyFromAppKey("a", "s2"));
}

records::ProfileSlot Slot(std::string id,
                          std::string name,
                          int64_t created,
                          int64_t renamed = 0) {
  records::ProfileSlot slot;
  slot.set_id(std::move(id));
  slot.set_name(std::move(name));
  slot.set_created_ms(created);
  slot.set_renamed_ms(renamed ? renamed : created);
  return slot;
}

records::ProfileSlots Slots(std::vector<records::ProfileSlot> list) {
  records::ProfileSlots slots;
  for (auto& slot : list) {
    *slots.add_slots() = std::move(slot);
  }
  return slots;
}

TEST(CloudSyncControllerTest, ProfilesMatchByName) {
  const auto slots = Slots({Slot("aa", "Work", 1), Slot("bb", "Personal", 2)});
  bool rename = true;
  // A new profile with an existing name joins it (trimmed, any case).
  EXPECT_EQ(CloudSyncController::ChooseSlot(slots, "", "", "  work ", &rename),
            "aa");
  EXPECT_FALSE(rename);
  // An unknown name needs a new slot.
  EXPECT_EQ(CloudSyncController::ChooseSlot(slots, "", "", "Games", &rename),
            "");
  // Its own slot, not renamed here: kept even if renamed elsewhere.
  EXPECT_EQ(
      CloudSyncController::ChooseSlot(slots, "aa", "Job", "Job", &rename),
      "aa");
  EXPECT_FALSE(rename);
  // Renamed here to a free name: the slot is renamed.
  EXPECT_EQ(
      CloudSyncController::ChooseSlot(slots, "aa", "Work", "Job", &rename),
      "aa");
  EXPECT_TRUE(rename);
  // Renamed here to another profile's name: the two are merged.
  EXPECT_EQ(CloudSyncController::ChooseSlot(slots, "aa", "Work", "Personal",
                                            &rename),
            "bb");
  EXPECT_FALSE(rename);
  // Its slot vanished: matched by name again.
  EXPECT_EQ(CloudSyncController::ChooseSlot(slots, "zz", "Personal",
                                            "Personal", &rename),
            "bb");
}

TEST(CloudSyncControllerTest, SlotVersionsMerge) {
  const auto merged = CloudSyncController::MergeSlots(
      {Slots({Slot("aa", "Work", 5), Slot("bb", "Home", 7)}),
       Slots({Slot("aa", "Job", 3, 9), Slot("cc", "Games", 8)})});
  ASSERT_EQ(merged.slots_size(), 3);
  EXPECT_EQ(merged.slots(0).id(), "aa");
  EXPECT_EQ(merged.slots(0).name(), "Job");      // Newest rename wins.
  EXPECT_EQ(merged.slots(0).created_ms(), 3);    // Oldest creation.
  EXPECT_EQ(merged.slots(1).id(), "bb");
  EXPECT_EQ(merged.slots(2).id(), "cc");
}

}  // namespace
}  // namespace bezel::cloudsync
