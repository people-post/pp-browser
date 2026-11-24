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
#include <limits>
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

  /** A coordinator started again over the same profile (a restart), with `ports` (none: the fixture's). */
  std::unique_ptr<RelayInboxCoordinator> Started(std::optional<RelayInboxCoordinator::Ports> ports = std::nullopt) {
    auto started = std::make_unique<RelayInboxCoordinator>(ports ? std::move(*ports) : Ports());
    started->SetInboxes({{kPreferred, preferred_}, {kOther, other_}}, dir_.string());
    return started;
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

// ---- an older app's cursor file: each inbox read on as before, then once from its start above a time floor ---------

/**
 * An older app's cursor file (no `registrations`): the preferred relay's cursor c1:1684 and, `with_other`, the other
 * relay's b-9; last written `age` ago (negative: in the future). That time, Unix ms (0: none).
 */
int64_t WriteOlderAppsFile(const std::filesystem::path& dir, const std::chrono::minutes age,
                           const bool with_other = true) {
  const auto path = dir / "relay_inbox_cursor.json";
  { // closed before its time is set and before the coordinator replaces it: Windows
    std::ofstream out(path, std::ios::trunc);
    out << R"({"schema_version":1,"account_id":")" << kMe << R"(","cursor":"c1:1684","cursor_relay":")"
        << RelayOriginKey(kPreferred) << R"(")";
    if (with_other) {
      out << R"(,"relays":{")" << RelayOriginKey(kOther) << R"(":"b-9"})";
    }
    out << "}";
  }
  std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - age);
  return RelayInboxCursorFileWrittenMs(dir.string()).value_or(0);
}

/** A minute: records sit this far either side of a floor (the file's time is read on the wall clock each time). */
constexpr int64_t kMinute = 60'000;

int64_t WallNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string At(const char* origin, const std::string& id) {
  return std::string(origin) + " " + id;
}

/** The re-read kept in the file for `origin`; none: no re-read there. */
std::optional<RelayInboxReread> KeptReread(const std::filesystem::path& dir, const char* origin) {
  const RelayInboxCursors kept = LoadRelayInboxCursors(dir.string(), kMe);
  const auto found = kept.rereads.find(RelayOriginKey(origin));
  return found == kept.rereads.end() ? std::nullopt : std::optional<RelayInboxReread>(found->second);
}

// The devices already deaf when this shipped, and every other install: their cursors were kept by an app that noted no
// mailbox. Each relay is polled on from its cursor as that app would (whether the registration there is known now or
// not), and once that answers an empty page it is read once from its start — then on from there, across restarts too.
TEST_F(RelayInboxCoordinatorTest, CursorsFromAFileThatNotedNoMailboxArePolledOnThenReadFromTheStartOnce) {
  WriteOlderAppsFile(dir_, std::chrono::hours(2));
  mailboxes_[kPreferred] = "reg-now"; // the other relay's: not known
  auto app = Started();
  preferred_->Queue({"m1", "m2"}, "c1:1690");
  other_->Queue({"m3"}, "b-10");
  app->Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1684") << "phase 1: the kept cursor, as the older app";
  EXPECT_EQ(other_->polled_with.back(), "b-9");
  const RelayInboxCursors noted = LoadRelayInboxCursors(dir_.string(), kMe);
  ASSERT_TRUE(noted.registrations);
  EXPECT_EQ(noted.registrations->at(RelayOriginKey(kPreferred)), "reg-now") << "its registration noted from now on";

  app->Sync(true, kMe); // empty pages: drained
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1690");
  EXPECT_EQ(other_->polled_with.back(), "b-10");
  preferred_->Queue({"m1"}, "c1:2");
  other_->Queue({"m3"}, "b-1");
  app->Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "") << "phase 2: from its start";
  EXPECT_EQ(other_->polled_with.back(), "");
  app->Sync(true, kMe); // empty pages: done
  EXPECT_FALSE(KeptReread(dir_, kPreferred));
  EXPECT_FALSE(KeptReread(dir_, kOther));

  auto restarted = Started();
  restarted->Sync(true, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:2") << "not again after a restart";
  EXPECT_EQ(other_->polled_with.back(), "b-1");
  EXPECT_EQ(std::count(preferred_->polled_with.begin(), preferred_->polled_with.end(), ""), 1);
  EXPECT_EQ(std::count(other_->polled_with.begin(), other_->polled_with.end(), ""), 1);
}

// The older app kept its cursor before a batch was taken in (KeepCursor, then the ingest lane): records behind that
// cursor may never have been taken in, though stored long before the file's last write. Phase 1 delivers them, whatever
// their time; phase 2 then skips what was stored before the floor (they among it: taken already) and takes the rest. A
// record with no store time is taken in. The cursor moves past what is skipped.
TEST_F(RelayInboxCoordinatorTest, ABacklogBehindTheOlderAppsCursorIsDelivered) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  const int64_t floor_ms = written - RelayInboxCoordinator::kRereadFloorSlackMs;
  auto app = Started();
  preferred_->QueueStored({{"backlog", written - 60 * kMinute}, {"since", written + kMinute}}, "c1:1690");
  app->Sync(false, kMe);
  app->Sync(false, kMe); // drained
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "backlog"), At(kPreferred, "since")}));

  preferred_->QueueStored({{"cleared-under-the-older-app", floor_ms - kMinute},
                           {"backlog", written - 60 * kMinute},
                           {"in-the-slack", floor_ms + kMinute},
                           {"since", written + kMinute},
                           {"no-store-time", std::nullopt}},
                          "c1:5");
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "backlog"), At(kPreferred, "since"),
                                              At(kPreferred, "in-the-slack"), At(kPreferred, "no-store-time")}))
      << "phase 2 hands on what is above the floor and phase 1 did not take";
  const auto reread = KeptReread(dir_, kPreferred);
  ASSERT_TRUE(reread);
  EXPECT_TRUE(reread->backfill);
  ASSERT_TRUE(reread->floor_ms);
  EXPECT_NEAR(static_cast<double>(*reread->floor_ms), static_cast<double>(floor_ms), 1000.0);
  EXPECT_EQ(LoadRelayInboxCursors(dir_.string(), kMe).preferred, "c1:5") << "past what was skipped";
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:5");
  EXPECT_FALSE(KeptReread(dir_, kPreferred)) << "an empty page: read through";
}

// A page whose records were all unreadable (the client skips them as it reads) comes back empty but with a cursor: the
// inbox goes on past it. Neither phase ends there — the backlog after it is delivered in phase 1 whatever its time, and
// phase 2 goes on to what was missed.
TEST_F(RelayInboxCoordinatorTest, APageOfUnreadableRecordsEndsNeitherPhase) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  const int64_t floor_ms = written - RelayInboxCoordinator::kRereadFloorSlackMs;
  auto app = Started();
  preferred_->Queue({}, "c1:1690"); // all unreadable
  preferred_->QueueStored({{"backlog", floor_ms - 60 * kMinute}}, "c1:1695");
  app->Sync(false, kMe);
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1690") << "phase 1 goes on past it";
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "backlog")}));
  auto reread = KeptReread(dir_, kPreferred);
  ASSERT_TRUE(reread);
  EXPECT_FALSE(reread->backfill);

  app->Sync(false, kMe);         // read through: phase 2
  preferred_->Queue({}, "c1:2"); // all unreadable
  preferred_->QueueStored({{"before-the-write", floor_ms - kMinute}, {"missed", written + kMinute}}, "c1:4");
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "");
  reread = KeptReread(dir_, kPreferred);
  ASSERT_TRUE(reread);
  EXPECT_TRUE(reread->backfill) << "phase 2 goes on past it";
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:2");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "backlog"), At(kPreferred, "missed")}));
  app->Sync(false, kMe);
  EXPECT_FALSE(KeptReread(dir_, kPreferred)) << "read through";
}

// What phase 1 took is noted (in memory) and not handed in again by phase 2, even above the floor: a late duplicate
// re-applies some group events. What phase 1 did not take still is. After a restart the note is gone: phase 2 hands it
// in again, and the receive pipeline drops it as seen.
TEST_F(RelayInboxCoordinatorTest, WhatPhaseOneTookIsNotHandedInAgainByPhaseTwo) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  auto app = Started();
  preferred_->QueueStored({{"taken-in-phase-1", written + kMinute}}, "c1:1690");
  app->Sync(false, kMe);
  app->Sync(false, kMe); // drained
  preferred_->QueueStored({{"taken-in-phase-1", written + kMinute}, {"not-taken", written + kMinute}}, "c1:2");
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "taken-in-phase-1"), At(kPreferred, "not-taken")}));

  auto restarted = Started(); // the note is lost
  preferred_->QueueStored({{"taken-in-phase-1", written + kMinute}}, "c1:3");
  restarted->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:2");
  EXPECT_EQ(taken_.back(), At(kPreferred, "taken-in-phase-1")) << "handed in again; the pipeline drops it as seen";
}

// The account was deaf: a fresh registration numbered its mailbox from the start, and the older app's cursor is past
// everything in it. Phase 1 answers an empty page at once; phase 2 reads the mailbox from its start and brings in what
// was stored since the file's last write — the messages that device missed.
TEST_F(RelayInboxCoordinatorTest, ADeafAccountsMissedMessagesArriveInPhaseTwo) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  const int64_t floor_ms = written - RelayInboxCoordinator::kRereadFloorSlackMs;
  mailboxes_[kPreferred] = "reg-new";
  auto app = Started();
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1684");
  EXPECT_TRUE(taken_.empty());
  preferred_->QueueStored({{"before-the-write", floor_ms - kMinute},
                           {"missed-1", written + kMinute},
                           {"missed-2", written + 30 * kMinute},
                           {"no-store-time", std::nullopt}},
                          "c1:3");
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "missed-1"), At(kPreferred, "missed-2"),
                                              At(kPreferred, "no-store-time")}));
}

// A file time in the future (the clock set back since, a copied profile) is not trusted: phase 1 still delivers what
// came in while the app was closed, from the kept cursor, and phase 2 takes everything — nothing is lost.
TEST_F(RelayInboxCoordinatorTest, AFileTimeInTheFutureLosesNothing) {
  const int64_t written = WriteOlderAppsFile(dir_, -std::chrono::minutes(60));
  ASSERT_GT(written, WallNowMs());
  auto app = Started();
  preferred_->QueueStored({{"arrived-while-closed", WallNowMs()}}, "c1:1690");
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1684");
  app->Sync(false, kMe); // drained
  const auto reread = KeptReread(dir_, kPreferred);
  ASSERT_TRUE(reread);
  EXPECT_FALSE(reread->floor_ms) << "no floor";
  preferred_->QueueStored({{"a-day-old", WallNowMs() - 24 * 60 * kMinute}, {"ancient", 1}}, "c1:2");
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "arrived-while-closed"), At(kPreferred, "a-day-old"),
                                              At(kPreferred, "ancient")}));
}

// This device's clock an hour ahead of the relay's: the file's time (ours) is an hour later than the store times
// (the relay's) of what came in after it. Phase 1 still delivers everything after the kept cursor; only phase 2's
// healing is narrowed (what was stored in that hour sits below its floor).
TEST_F(RelayInboxCoordinatorTest, ADeviceClockAheadOfTheRelayStillGetsEverythingAfterTheCursor) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::minutes(0));
  const int64_t relay_written = written - 60 * kMinute; // the same moment on the relay's clock
  auto app = Started();
  preferred_->QueueStored({{"after-1", relay_written + kMinute}, {"after-2", relay_written + 20 * kMinute}}, "c1:1690");
  app->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1684");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "after-1"), At(kPreferred, "after-2")}));
  app->Sync(false, kMe); // drained
  preferred_->QueueStored({{"after-1", relay_written + kMinute}, {"after-2", relay_written + 20 * kMinute}}, "c1:2");
  app->Sync(false, kMe);
  EXPECT_EQ(taken_.size(), 2u) << "phase 2 skips them (below its floor by the skew): delivered once, in phase 1";
}

// No time to trust for the older app's file — none read, or one at an extreme — means no floor: phase 2 takes everything
// (old chats may come back once; nothing is lost), and nothing traps on the arithmetic.
TEST_F(RelayInboxCoordinatorTest, AnUnreadableOrExtremeFileTimeMeansPhaseTwoTakesEverything) {
  const std::vector<std::optional<int64_t>> times = {std::nullopt, std::numeric_limits<int64_t>::min(), 0,
                                                     RelayInboxCoordinator::kRereadFloorSlackMs,
                                                     std::numeric_limits<int64_t>::max()};
  for (size_t i = 0; i < times.size(); ++i) {
    SCOPED_TRACE(i);
    WriteOlderAppsFile(dir_, std::chrono::hours(2), false);
    RelayInboxCoordinator::Ports ports = Ports();
    ports.cursors_written_ms = [time = times[i]](const std::string&) { return time; };
    auto app = Started(std::move(ports));
    app->Sync(false, kMe); // phase 1: an empty page at once
    const auto reread = KeptReread(dir_, kPreferred);
    ASSERT_TRUE(reread);
    EXPECT_FALSE(reread->floor_ms);
    const std::string id = "ancient-" + std::to_string(i);
    preferred_->QueueStored({{id, 1}}, "c1:" + std::to_string(i));
    app->Sync(false, kMe);
    EXPECT_EQ(preferred_->polled_with.back(), "");
    ASSERT_FALSE(taken_.empty());
    EXPECT_EQ(taken_.back(), At(kPreferred, id));
  }
}

// Stopped during phase 1: it goes on from the cursor it kept, unfiltered, then on to phase 2.
TEST_F(RelayInboxCoordinatorTest, ARestartInPhaseOneGoesOnUnfilteredFromTheSavedCursor) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  {
    auto app = Started();
    preferred_->QueueStored({{"backlog-1", written - 60 * kMinute}}, "c1:1690");
    app->Sync(false, kMe);
  }
  auto restarted = Started();
  preferred_->QueueStored({{"backlog-2", written - 60 * kMinute}}, "c1:1695");
  restarted->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1690") << "on from where it stopped";
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "backlog-1"), At(kPreferred, "backlog-2")}));
  restarted->Sync(false, kMe); // drained
  restarted->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "") << "then phase 2";
}

// Stopped during phase 2: it goes on from phase 2's cursor, still above the floor read back from the file (whose own
// time is new by then), until an empty page; after that an old record is taken in as any.
TEST_F(RelayInboxCoordinatorTest, ARestartInPhaseTwoGoesOnFilteredFromItsCursor) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  const int64_t floor_ms = written - RelayInboxCoordinator::kRereadFloorSlackMs;
  {
    auto app = Started();
    app->Sync(false, kMe); // drained at once
    preferred_->QueueStored({{"old-1", floor_ms - kMinute}, {"new-1", written + kMinute}}, "c1:3");
    app->Sync(false, kMe);
    EXPECT_EQ(preferred_->polled_with.back(), "");
  }
  auto restarted = Started();
  preferred_->QueueStored({{"old-2", floor_ms - kMinute}, {"new-2", written + kMinute}}, "c1:6");
  restarted->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:3") << "on from phase 2's cursor";
  restarted->Sync(false, kMe); // an empty page: read through
  EXPECT_FALSE(KeptReread(dir_, kPreferred));
  preferred_->QueueStored({{"old-3", floor_ms - kMinute}}, "c1:7");
  restarted->Sync(false, kMe);
  EXPECT_EQ(taken_,
            (std::vector<std::string>{At(kPreferred, "new-1"), At(kPreferred, "new-2"), At(kPreferred, "old-3")}));
}

// An older app run in between writes the file again without the re-read's keys: the next run takes it for an older
// app's file again — phase 1 from that app's cursor, then phase 2 — and loses nothing.
TEST_F(RelayInboxCoordinatorTest, AnOlderAppWritingTheFileAgainStartsTheReReadOverSafely) {
  const int64_t written = WriteOlderAppsFile(dir_, std::chrono::hours(2));
  const int64_t floor_ms = written - RelayInboxCoordinator::kRereadFloorSlackMs;
  {
    auto app = Started();
    app->Sync(false, kMe); // drained at once
    preferred_->QueueStored({{"new-1", written + kMinute}}, "c1:3");
    app->Sync(false, kMe); // phase 2 under way
  }
  { // the older app's write: the cursors only
    std::ofstream out(dir_ / "relay_inbox_cursor.json", std::ios::trunc);
    out << R"({"schema_version":1,"account_id":")" << kMe << R"(","cursor":"c1:9","cursor_relay":")"
        << RelayOriginKey(kPreferred) << R"("})";
  }
  auto again = Started();
  preferred_->QueueStored({{"behind-its-cursor", floor_ms - kMinute}}, "c1:10");
  again->Sync(false, kMe);
  EXPECT_EQ(preferred_->polled_with.back(), "c1:9") << "phase 1 from the older app's cursor";
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "new-1"), At(kPreferred, "behind-its-cursor")}));
  const auto reread = KeptReread(dir_, kPreferred);
  ASSERT_TRUE(reread);
  EXPECT_FALSE(reread->backfill);
}

// A relay the older app's file holds no cursor for has nothing to read on from: it is read from its start, unfiltered,
// as on any install, with no re-read state.
TEST_F(RelayInboxCoordinatorTest, ARelayWithNoCursorInTheOlderAppsFileIsReadAsBefore) {
  WriteOlderAppsFile(dir_, std::chrono::hours(2), false);
  auto app = Started();
  other_->QueueStored({{"ancient", 1}}, "b-1");
  app->Sync(true, kMe);
  EXPECT_EQ(other_->polled_with.back(), "");
  EXPECT_EQ(preferred_->polled_with.back(), "c1:1684");
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kOther, "ancient")}));
  EXPECT_FALSE(KeptReread(dir_, kOther));
  EXPECT_TRUE(KeptReread(dir_, kPreferred));
}

// A fresh install (no file) and a file that notes the registrations (this app's) have no re-read, however old a record.
TEST_F(RelayInboxCoordinatorTest, NoFileOrACurrentFileSkipsNothing) {
  preferred_->QueueStored({{"ancient", 1}}, "c1:1");
  coordinator_->Sync(false, kMe);
  EXPECT_TRUE(LoadRelayInboxCursors(dir_.string(), kMe).rereads.empty());

  auto app = Started();
  preferred_->QueueStored({{"ancient-2", 1}}, "c1:2");
  app->Sync(false, kMe);

  EXPECT_EQ(preferred_->polled_with.back(), "c1:1") << "its cursor kept";
  EXPECT_EQ(taken_, (std::vector<std::string>{At(kPreferred, "ancient"), At(kPreferred, "ancient-2")}));
  EXPECT_TRUE(LoadRelayInboxCursors(dir_.string(), kMe).rereads.empty());
}

TEST_F(RelayInboxCoordinatorTest, NoInboxesPollsNothing) {
  coordinator_->SetInboxes({}, dir_.string());
  coordinator_->Sync(true, kMe);
  EXPECT_TRUE(preferred_->polled_with.empty());
}

} // namespace
} // namespace pbr
