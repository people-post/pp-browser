#include "domain/delivery/RelayInboxCoordinator.h"

#include "foundation/error/AppError.h"
#include "lib/net/RelayInboxCursor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
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

  Roe<void> Send(const RelayEnvelope&) override { return {}; }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string& cursor) override {
    std::lock_guard lock(mutex_);
    polled_with.push_back(cursor);
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

private:
  std::mutex mutex_;
  std::vector<RelayPollResult> queued_;
  bool fail_ = false;
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
    return ports;
  }

  std::filesystem::path dir_;
  std::shared_ptr<FakeInbox> preferred_ = std::make_shared<FakeInbox>();
  std::shared_ptr<FakeInbox> other_ = std::make_shared<FakeInbox>();
  std::set<std::string> registered_{kOther};
  std::vector<std::string> taken_;
  std::vector<std::string> outcomes_;
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

TEST_F(RelayInboxCoordinatorTest, NoInboxesPollsNothing) {
  coordinator_->SetInboxes({}, dir_.string());
  coordinator_->Sync(true, kMe);
  EXPECT_TRUE(preferred_->polled_with.empty());
}

} // namespace
} // namespace pbr
