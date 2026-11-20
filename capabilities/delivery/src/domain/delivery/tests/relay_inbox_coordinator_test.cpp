#include "domain/delivery/RelayInboxCoordinator.h"

#include "foundation/error/AppError.h"
#include "lib/net/RelayInboxCursor.h"

#include <gtest/gtest.h>

#include <filesystem>
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
    return ports;
  }

  std::filesystem::path dir_;
  std::shared_ptr<FakeInbox> preferred_ = std::make_shared<FakeInbox>();
  std::shared_ptr<FakeInbox> other_ = std::make_shared<FakeInbox>();
  std::set<std::string> registered_{kOther};
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

TEST_F(RelayInboxCoordinatorTest, NoInboxesPollsNothing) {
  coordinator_->SetInboxes({}, dir_.string());
  coordinator_->Sync(true, kMe);
  EXPECT_TRUE(preferred_->polled_with.empty());
}

} // namespace
} // namespace pbr
