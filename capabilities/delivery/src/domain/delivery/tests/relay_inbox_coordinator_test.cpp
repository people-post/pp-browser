#include "domain/delivery/RelayInboxCoordinator.h"

#include "foundation/error/AppError.h"
#include "lib/net/RelayInboxCursor.h"
#include "lib/net/RelayOrigins.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Receiving on every relay of ours (multi-relay R2; SERVICE_ENDPOINTS § Several relays): the preferred relay at the
// app's pace, the others every kOtherRelaysEverySyncs-th sync and on every forced one, each with its own cursor, acked
// on the relay a batch came from, and another relay only once we are registered there.

constexpr const char kMe[] = "account:me";
constexpr const char kPreferred[] = "https://a.example/api/relay";
constexpr const char kOther[] = "https://b.example/api/relay";

/** An inbox: each poll answers its queued batch (then nothing), and records what it was asked. */
class FakeInbox : public IRelayClient {
public:
  void Queue(std::vector<std::string> message_ids, std::string next_cursor) {
    std::lock_guard lock(mutex_);
    RelayPollResult result;
    for (auto& id : message_ids) {
      RelayEnvelope envelope;
      envelope.message_id = std::move(id);
      result.messages.push_back(std::move(envelope));
    }
    result.next_cursor = std::move(next_cursor);
    queued_.push_back(std::move(result));
  }
  /** As Queue, each record with the time the relay stored it (Unix ms; none: the relay gave none). */
  void QueueStored(std::vector<std::pair<std::string, std::optional<int64_t>>> records, std::string next_cursor) {
    std::lock_guard lock(mutex_);
    RelayPollResult result;
    for (auto& [id, stored_at] : records) {
      RelayEnvelope envelope;
      envelope.message_id = std::move(id);
      envelope.relay_created_at_ms = stored_at;
      result.messages.push_back(std::move(envelope));
    }
    result.next_cursor = std::move(next_cursor);
    queued_.push_back(std::move(result));
  }
  void Fail(bool fail) { fail_ = fail; }
  /** Every poll is refused with `refusal` (none: answered as queued). */
  void Refuse(std::optional<Error> refusal) {
    std::lock_guard lock(mutex_);
    refusal_ = std::move(refusal);
  }

  Roe<void> Send(const RelayEnvelope&) override { return {}; }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string& cursor) override {
    if (auto hook = std::exchange(on_poll, nullptr)) {
      hook(); // once, while the poll is under way (before it answers)
    }
    std::lock_guard lock(mutex_);
    polled_with.push_back(cursor);
    if (refusal_) {
      return *refusal_;
    }
    if (fail_) {
      return AppError::Network(Err::Network::ConnectFailed, "down");
    }
    if (queued_.empty()) {
      return RelayPollResult{};
    }
    RelayPollResult next = queued_.front();
    queued_.erase(queued_.begin());
    return next;
  }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string& cursor) override {
    std::lock_guard lock(mutex_);
    acked.push_back(cursor);
    return RelayDeleteResult{};
  }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return RelayDeleteResult{}; }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return ChatHistoryResponse{}; }

  std::vector<std::string> polled_with;
  std::vector<std::string> acked;
  std::function<void()> on_poll;

private:
  std::mutex mutex_;
  std::vector<RelayPollResult> queued_;
  bool fail_ = false;
  std::optional<Error> refusal_;
};

class RelayInboxCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp-relay-inbox-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir_);
    coordinator_ = std::make_unique<RelayInboxCoordinator>(Ports());
    coordinator_->SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  }
  void TearDown() override {
    coordinator_.reset();
    std::filesystem::remove_all(dir_);
  }

  RelayInboxCoordinator::Ports Ports() {
    RelayInboxCoordinator::Ports ports;
    ports.poll = [](std::function<void()> fn) { fn(); };
    ports.ingest = [](std::function<void()> fn) { fn(); };
    ports.ack = [](std::function<void()> fn) { fn(); };
    ports.take = [this](const RelayInboxCoordinator::Batch& batch, const std::string&) {
      for (const auto& envelope : batch.messages) {
        taken_.push_back(batch.origin + " " + envelope.message_id);
      }
    };
    ports.polled = [this](const std::string& origin, bool, const Error* failure) {
      outcomes_.push_back(origin + (failure ? " failed" : " ok"));
    };
    ports.ready = [this](const std::string& origin) { return registered_.contains(origin); };
    ports.now_ms = [this]() { return now_ms_; };
    ports.mailbox = [this](const std::string& origin, bool) {
      const auto it = mailboxes_.find(origin);
      return it == mailboxes_.end() ? std::string() : it->second;
    };
    return ports;
  }

  std::filesystem::path dir_;
  std::shared_ptr<FakeInbox> preferred_ = std::make_shared<FakeInbox>();
  std::shared_ptr<FakeInbox> other_ = std::make_shared<FakeInbox>();
  std::set<std::string> registered_{kOther};
  /** The registration held at each relay (Ports::mailbox); none: unknown. */
  std::map<std::string, std::string> mailboxes_;
  std::vector<std::string> taken_;
  std::vector<std::string> outcomes_;
  int64_t now_ms_ = 1'000'000;
  std::unique_ptr<RelayInboxCoordinator> coordinator_;
};

TEST_F(RelayInboxCoordinatorTest, ThePreferredRelayIsPolledEverySyncAndTheOthersEveryFourth) {
  for (int i = 1; i <= 2 * RelayInboxCoordinator::kOtherRelaysEverySyncs; ++i) {
    coordinator_->Sync(false, kMe);
  }
  EXPECT_EQ(preferred_->polled_with.size(), 2u * RelayInboxCoordinator::kOtherRelaysEverySyncs);
  EXPECT_EQ(other_->polled_with.size(), 2u);
}

TEST_F(RelayInboxCoordinatorTest, AForcedSyncPollsEveryRelay) {
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.size(), 1u);
  EXPECT_EQ(other_->polled_with.size(), 1u) << "a push may come from any relay holding mail";
}

TEST_F(RelayInboxCoordinatorTest, AnotherRelayIsPolledOnlyOnceWeAreRegisteredThere) {
  registered_.clear();
  coordinator_->Sync(true, kMe);
  EXPECT_TRUE(other_->polled_with.empty());
  registered_.insert(kOther);
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(other_->polled_with.size(), 1u);
}

TEST_F(RelayInboxCoordinatorTest, EachBatchIsTakenAndAckedOnTheRelayItCameFrom) {
  preferred_->Queue({"m1"}, "a-1");
  other_->Queue({"m1", "m2"}, "b-1");
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(taken_, std::vector<std::string>(
                        {std::string(kPreferred) + " m1", std::string(kOther) + " m1", std::string(kOther) + " m2"}))
      << "the same message through both relays reaches the pipeline twice; it drops the second by its id";
  EXPECT_EQ(preferred_->acked, std::vector<std::string>({"a-1"}));
  EXPECT_EQ(other_->acked, std::vector<std::string>({"b-1"}));
}

TEST_F(RelayInboxCoordinatorTest, EachRelayPollsOnFromItsOwnCursorAcrossARestart) {
  preferred_->Queue({"m1"}, "a-1");
  other_->Queue({"m2"}, "b-1");
  coordinator_->Sync(true, kMe);
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "a-1");
  EXPECT_EQ(other_->polled_with.back(), "b-1");

  RelayInboxCoordinator again(Ports());
  again.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  again.Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "a-1");
  EXPECT_EQ(other_->polled_with.back(), "b-1");
  const RelayInboxCursors kept = LoadRelayInboxCursors(dir_.string(), kMe);
  EXPECT_EQ(kept.preferred, "a-1");
  EXPECT_EQ(kept.others.size(), 1u);
}

TEST_F(RelayInboxCoordinatorTest, ARelayThatFailsIsReportedAndTheOthersStillDeliver) {
  preferred_->Fail(true);
  other_->Queue({"m1"}, "b-1");
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(outcomes_, std::vector<std::string>({std::string(kPreferred) + " failed", std::string(kOther) + " ok"}));
  EXPECT_EQ(taken_, std::vector<std::string>({std::string(kOther) + " m1"}));
}

// T14: a relay refusing the poll for good (426, 400 relay_upgrade_required / unsupported_sign_version, wrong_audience
// at the PeerId it presents) is not asked every 2 s: it waits kRefusedFirstMs, doubling to kRefusedMaxMs, forced syncs
// too; the others go on, and an answer ends the back-off.
TEST_F(RelayInboxCoordinatorTest, APermanentRefusalBacksThePollOff) {
  for (const Error& refusal : {AppError::Auth(Err::Auth::ClientUpgradeRequired, "426"),
                               AppError::Auth(Err::Auth::RelayUpgradeRequired, "relay_upgrade_required"),
                               AppError::Auth(Err::Auth::WrongAudience, "wrong_audience")}) {
    coordinator_ = std::make_unique<RelayInboxCoordinator>(Ports());
    coordinator_->SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
    preferred_->polled_with.clear();
    other_->polled_with.clear();
    preferred_->Refuse(refusal);
    coordinator_->Sync(false, kMe);
    ASSERT_EQ(preferred_->polled_with.size(), 1u) << refusal.message;
    for (int i = 0; i < 20; ++i) {
      now_ms_ += 2'000; // the app's pace
      coordinator_->Sync(i % 5 == 0, kMe);
    }
    EXPECT_EQ(preferred_->polled_with.size(), 1u) << "not again within the first minute, forced or not";
    EXPECT_GT(other_->polled_with.size(), 1u) << "the other relays go on";
    now_ms_ += RelayInboxCoordinator::kRefusedFirstMs;
    coordinator_->Sync(false, kMe);
    EXPECT_EQ(preferred_->polled_with.size(), 2u) << "asked again after the back-off";
    now_ms_ += RelayInboxCoordinator::kRefusedFirstMs; // the wait doubled: not yet
    coordinator_->Sync(false, kMe);
    EXPECT_EQ(preferred_->polled_with.size(), 2u);
    now_ms_ += RelayInboxCoordinator::kRefusedFirstMs;
    coordinator_->Sync(false, kMe);
    EXPECT_EQ(preferred_->polled_with.size(), 3u);
    preferred_->Refuse(std::nullopt);
    now_ms_ += RelayInboxCoordinator::kRefusedMaxMs;
    coordinator_->Sync(false, kMe);
    coordinator_->Sync(false, kMe);
    EXPECT_EQ(preferred_->polled_with.size(), 5u) << "an answer ends the back-off: every sync again";
  }
}

TEST_F(RelayInboxCoordinatorTest, AnOrdinaryFailureIsNotBackedOff) {
  preferred_->Fail(true);
  for (int i = 0; i < 5; ++i) {
    coordinator_->Sync(false, kMe);
  }
  EXPECT_EQ(preferred_->polled_with.size(), 5u) << "unreachable is the network's: polled at the app's pace";
  preferred_->Fail(false);
  preferred_->Refuse(AppError::Auth(Err::Auth::NotRegistered, "not_registered"));
  coordinator_->Sync(false, kMe);
  coordinator_->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.size(), 7u) << "a lost registration heals: not backed off";
}

TEST_F(RelayInboxCoordinatorTest, TheBackOffStopsAtItsLongest) {
  preferred_->Refuse(AppError::Auth(Err::Auth::ClientUpgradeRequired, "426"));
  coordinator_->Sync(false, kMe);
  for (int i = 0; i < 12; ++i) {
    now_ms_ += RelayInboxCoordinator::kRefusedMaxMs;
    coordinator_->Sync(false, kMe);
  }
  EXPECT_EQ(preferred_->polled_with.size(), 13u) << "at most every kRefusedMaxMs, never further apart";
}

// My relays (MR006): a relay made preferred polls on from its own cursor; a relay that leaves the list loses its
// cursor, in the file too.
TEST_F(RelayInboxCoordinatorTest, ARelayMadePreferredPollsOnFromItsOwnCursor) {
  preferred_->Queue({"m1"}, "a-1");
  other_->Queue({"m2"}, "b-1");
  coordinator_->Sync(true, kMe);
  coordinator_->SetInboxes({{kOther, other_}, {kPreferred, preferred_}}, dir_.string());
  registered_ = {kPreferred};
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(other_->polled_with.back(), "b-1");
  EXPECT_EQ(preferred_->polled_with.back(), "a-1");

  RelayInboxCoordinator again(Ports());
  again.SetInboxes({{kOther, other_}, {kPreferred, preferred_}}, dir_.string());
  again.Sync(true, kMe);
  EXPECT_EQ(other_->polled_with.back(), "b-1") << "across a restart too";
  EXPECT_EQ(preferred_->polled_with.back(), "a-1");
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), kMe).preferred_relay, "https://b.example/api/relay");
}

TEST_F(RelayInboxCoordinatorTest, ARelayLeavingTheListLosesItsCursor) {
  other_->Queue({"m2"}, "b-1");
  coordinator_->Sync(true, kMe);
  ASSERT_EQ(LoadRelayInboxCursors(dir_.string(), kMe).others.size(), 1u);
  coordinator_->SetInboxes({{kPreferred, preferred_}}, dir_.string());
  EXPECT_TRUE(LoadRelayInboxCursors(dir_.string(), kMe).others.empty());
  coordinator_->SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(other_->polled_with.back(), "") << "added again: from its oldest message";
}

// T7: a list change that leaves an inbox as it was keeps it: a poll under way there keeps the cursor it brings back.
TEST_F(RelayInboxCoordinatorTest, AnUnchangedInboxKeepsItsPollUnderWayAcrossAListChange) {
  preferred_->Queue({"m1"}, "a-1");
  auto third = std::make_shared<FakeInbox>();
  preferred_->on_poll = [this, third]() {
    coordinator_->SetInboxes({{kPreferred, preferred_}, {kOther, other_}, {"https://c.example/api/relay", third}},
                             dir_.string());
  };
  coordinator_->Sync(false, kMe);
  coordinator_->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "a-1") << "the cursor the poll under way brought back is kept";
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), kMe).preferred, "a-1");
}

TEST_F(RelayInboxCoordinatorTest, ACursorFromBeforeMyRelaysIsThePreferredRelays) {
  SaveRelayInboxCursors(dir_.string(), kMe,
                        RelayInboxCursors{.preferred = "a-old", .preferred_relay = "", .others = {}});
  RelayInboxCoordinator again(Ports());
  again.SetInboxes({{kPreferred, preferred_}}, dir_.string());
  again.Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "a-old");
}

// A fresh registration is a new mailbox at the relay, numbered from the start (2026-11-22: after a revoke the account's
// devices polled the new mailbox with the old one's cursor and received nothing): a cursor counts in the registration
// it was kept for; the same registration (a renewal) keeps it.
TEST_F(RelayInboxCoordinatorTest, ANewRegistrationIsANewMailboxReadFromItsStart) {
  mailboxes_[kPreferred] = "reg-1";
  preferred_->Queue({"m1"}, "c1:1684");
  coordinator_->Sync(false, kMe);
  coordinator_->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1684") << "the same registration keeps its cursor";

  mailboxes_[kPreferred] = "reg-2"; // registered again: the relay's new mailbox
  preferred_->Queue({"n1"}, "c1:1");
  coordinator_->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "") << "the new mailbox from its start";
  coordinator_->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1") << "then on in it";
  EXPECT_EQ(taken_, std::vector<std::string>({std::string(kPreferred) + " m1", std::string(kPreferred) + " n1"}));

  RelayInboxCoordinator again(Ports());
  again.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  again.Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1") << "kept across a restart, with its registration";
  const RelayInboxCursors kept = LoadRelayInboxCursors(dir_.string(), kMe);
  EXPECT_EQ(kept.preferred, "c1:1");
  ASSERT_TRUE(kept.registrations);
  EXPECT_EQ(kept.registrations->at(RelayOriginKey(kPreferred)), "reg-2");
}

TEST_F(RelayInboxCoordinatorTest, EachRelaysCursorFollowsItsOwnRegistration) {
  mailboxes_ = {{kPreferred, "reg-a"}, {kOther, "reg-b1"}};
  preferred_->Queue({"m1"}, "a-1");
  other_->Queue({"m2"}, "b-1");
  coordinator_->Sync(true, kMe);
  mailboxes_[kOther] = "reg-b2";
  coordinator_->Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "a-1");
  EXPECT_EQ(other_->polled_with.back(), "") << "only the relay that registered us again starts over";
}

// The devices already deaf when this shipped: their cursors were kept by an app that noted no mailbox — whether the
// registration there is known now or not, each inbox is read once from its start (the receive pipeline drops what was
// taken already), then on from there, across restarts too.
TEST_F(RelayInboxCoordinatorTest, CursorsFromAFileThatNotedNoMailboxAreReadFromTheStartOnce) {
  std::ofstream(dir_ / "relay_inbox_cursor.json")
      << R"({"schema_version":1,"account_id":")" << kMe << R"(","cursor":"c1:1684","cursor_relay":")"
      << RelayOriginKey(kPreferred) << R"(","relays":{")" << RelayOriginKey(kOther) << R"(":"b-9"}})";
  mailboxes_[kPreferred] = "reg-now"; // the other relay's: not known
  RelayInboxCoordinator again(Ports());
  again.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  preferred_->Queue({"m1", "m2"}, "c1:2");
  other_->Queue({"m3"}, "b-1");
  again.Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "");
  EXPECT_EQ(other_->polled_with.back(), "") << "every cursor of that file";
  again.Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:2") << "once";
  EXPECT_EQ(other_->polled_with.back(), "b-1");

  RelayInboxCoordinator third(Ports());
  third.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  third.Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:2") << "not again after a restart";
  EXPECT_EQ(other_->polled_with.back(), "b-1");
  EXPECT_EQ(std::count(preferred_->polled_with.begin(), preferred_->polled_with.end(), ""), 1);
  EXPECT_EQ(std::count(other_->polled_with.begin(), other_->polled_with.end(), ""), 1);
}

// A cursor kept while the registration there was not known (no id answered yet) is used as it is; once a registration
// is known it counts in that one, and when it changes the inbox starts over.
TEST_F(RelayInboxCoordinatorTest, ACursorKeptWhileTheRegistrationWasNotKnownStartsOverOnceItIs) {
  preferred_->Queue({"m1"}, "c1:40");
  coordinator_->Sync(false, kMe);
  coordinator_->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:40") << "not known: kept";
  mailboxes_[kPreferred] = "reg-1"; // registered (again): the relay's answer names it
  coordinator_->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "");
}

// ---- the re-read of an older app's file skips what the relay stored before the file was last written ----------------

/** An older app's cursor file (cursors for both relays, no `registrations`), last written `age` ago; that time, Unix ms. */
int64_t WriteOlderAppsFile(const std::filesystem::path& dir, const std::chrono::minutes age) {
  const auto path = dir / "relay_inbox_cursor.json";
  { // closed before its time is set and before the coordinator replaces it: Windows
    std::ofstream out(path, std::ios::trunc);
    out << R"({"schema_version":1,"account_id":")" << kMe << R"(","cursor":"c1:1684","cursor_relay":")"
        << RelayOriginKey(kPreferred) << R"(","relays":{")" << RelayOriginKey(kOther) << R"(":"b-9"}})";
  }
  std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - age);
  const auto written = RelayInboxCursorFileWrittenMs(dir.string());
  EXPECT_TRUE(written);
  return written.value_or(0);
}

constexpr int64_t kMinute = 60'000;

std::string At(const char* origin, const std::string& id) {
  return std::string(origin) + " " + id;
}

// Every record the device took in from an older app's inbox was stored before that app last wrote its file (it wrote it
// as the cursor moved); a deaf device's missed messages were stored after (its cursor stood still). So the once-only
// re-read skips what was stored before the file's last write (less kRereadFloorSlackMs): chats cleared or deleted under
// the older app, which kept no removed ids, do not come back; the missed messages arrive. A record with no store time is
// taken in. The cursor moves past what is skipped; the registration is noted from now on.
TEST_F(RelayInboxCoordinatorTest, AnOlderAppsFileIsReadAgainOnlyFromTheTimeItWasLastWritten) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  const int64_t floor_ms = written - RelayInboxCoordinator::kRereadFloorSlackMs;
  mailboxes_[kPreferred] = "reg-now";
  RelayInboxCoordinator again(Ports());
  again.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  // A minute either side of the floor: the file's time is read on the wall clock each time, a millisecond apart.
  preferred_->QueueStored({{"cleared-under-the-older-app", floor_ms - kMinute},
                           {"in-the-slack", floor_ms + kMinute},
                           {"missed", written + kMinute},
                           {"no-store-time", std::nullopt}},
                          "c1:4");

  again.Sync(false, kMe);

  EXPECT_EQ(preferred_->polled_with.back(), "") << "from its start";
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "in-the-slack"), At(kPreferred, "missed"),
                                              At(kPreferred, "no-store-time")}));
  RelayInboxCursors kept = LoadRelayInboxCursors(dir_.string(), kMe);
  EXPECT_EQ(kept.preferred, "c1:4") << "past what was skipped";
  ASSERT_TRUE(kept.registrations);
  EXPECT_EQ(kept.registrations->at(RelayOriginKey(kPreferred)), "reg-now");
  ASSERT_EQ(kept.reread_floors_ms.size(), 2u) << "each relay whose cursor went";
  EXPECT_NEAR(static_cast<double>(kept.reread_floors_ms.at(RelayOriginKey(kPreferred))), static_cast<double>(floor_ms),
              1000.0);
  EXPECT_EQ(kept.reread_floors_ms.at(RelayOriginKey(kOther)), kept.reread_floors_ms.at(RelayOriginKey(kPreferred)));
  const std::map<std::string, int64_t> floors = kept.reread_floors_ms;

  again.Sync(false, kMe); // an empty page: read through
  EXPECT_EQ(preferred_->polled_with.back(), "c1:4");
  kept = LoadRelayInboxCursors(dir_.string(), kMe);
  EXPECT_EQ(kept.reread_floors_ms,
            (std::map<std::string, int64_t>{{RelayOriginKey(kOther), floors.at(RelayOriginKey(kOther))}}))
      << "the other relay's re-read is still under way";
}

// The app stopped in the middle of the re-read: the floor is read back from the file (whose own time is new by then)
// and skips on until a page comes back empty; after that an old record is taken in as any.
TEST_F(RelayInboxCoordinatorTest, ARestartDuringTheReReadKeepsItsFloorUntilAnEmptyPage) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  const int64_t floor_ms = written - RelayInboxCoordinator::kRereadFloorSlackMs;
  {
    RelayInboxCoordinator first(Ports());
    first.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
    preferred_->QueueStored({{"old-1", floor_ms - kMinute}, {"new-1", written + kMinute}}, "c1:2");
    first.Sync(false, kMe);
  }
  RelayInboxCoordinator second(Ports());
  second.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  preferred_->QueueStored({{"old-2", floor_ms - kMinute}, {"new-2", written + kMinute}}, "c1:4");
  second.Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:2") << "on from where it stopped";

  second.Sync(false, kMe); // an empty page: read through
  EXPECT_EQ(preferred_->polled_with.back(), "c1:4");
  EXPECT_FALSE(LoadRelayInboxCursors(dir_.string(), kMe).reread_floors_ms.contains(RelayOriginKey(kPreferred)));
  preferred_->QueueStored({{"old-3", floor_ms - kMinute}}, "c1:5");
  second.Sync(false, kMe);

  RelayInboxCoordinator third(Ports());
  third.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  preferred_->QueueStored({{"old-4", floor_ms - kMinute}}, "c1:6");
  third.Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:5");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "new-1"), At(kPreferred, "new-2"), At(kPreferred, "old-3"),
                                              At(kPreferred, "old-4")}));
}

// With no time for the older app's file there is no floor: every inbox is read again whole, as before.
TEST_F(RelayInboxCoordinatorTest, AnOlderAppsFileWhoseTimeCannotBeReadIsReadAgainWhole) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  RelayInboxCoordinator::Ports ports = Ports();
  ports.cursors_written_ms = [](const std::string&) { return std::optional<int64_t>(); };
  RelayInboxCoordinator again(std::move(ports));
  again.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  preferred_->QueueStored({{"a-day-before", written - 24 * 3600 * 1000}, {"no-store-time", std::nullopt}}, "c1:2");

  again.Sync(false, kMe);

  EXPECT_EQ(preferred_->polled_with.back(), "");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "a-day-before"), At(kPreferred, "no-store-time")}));
  EXPECT_TRUE(LoadRelayInboxCursors(dir_.string(), kMe).reread_floors_ms.empty());
}

// A fresh install (no file) and a file that notes the registrations (this app's) skip nothing, however old.
TEST_F(RelayInboxCoordinatorTest, NoFileOrACurrentFileSkipsNothing) {
  preferred_->QueueStored({{"ancient", 1}}, "c1:1");
  coordinator_->Sync(false, kMe);
  EXPECT_TRUE(LoadRelayInboxCursors(dir_.string(), kMe).reread_floors_ms.empty());

  RelayInboxCoordinator again(Ports());
  again.SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
  preferred_->QueueStored({{"ancient-2", 1}}, "c1:2");
  again.Sync(false, kMe);

  EXPECT_EQ(preferred_->polled_with.back(), "c1:1") << "its cursor kept";
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "ancient"), At(kPreferred, "ancient-2")}));
  EXPECT_TRUE(LoadRelayInboxCursors(dir_.string(), kMe).reread_floors_ms.empty());
}

TEST_F(RelayInboxCoordinatorTest, NoInboxesPollsNothing) {
  coordinator_->SetInboxes({}, dir_.string());
  coordinator_->Sync(true, kMe);
  EXPECT_TRUE(preferred_->polled_with.empty());
}

} // namespace
} // namespace pbr
