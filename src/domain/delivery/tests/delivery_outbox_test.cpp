#include "domain/delivery/DeliveryOutbox.h"

#include "foundation/runtime/AppRuntime.h"
#include "lib/net/OrgBackendClients.h"
#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace pbr {
namespace {

/** A relay that refuses the first `failures` sends. */
class FakeRelay final : public IRelayClient {
public:
  explicit FakeRelay(int failures) : failures_(failures) {}
  Roe<void> Send(const RelayEnvelope& envelope) override {
    std::lock_guard lock(mu_);
    sent.push_back(envelope.message_id);
    if (failures_ > 0) {
      --failures_;
      return Error("relay unreachable");
    }
    return {};
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }

  std::vector<std::string> sent;

private:
  std::mutex mu_;
  int failures_;
};

class DeliveryOutboxTest : public ::testing::Test {
protected:
  void SetUp() override { AppRuntime::Initialize(); }
  void TearDown() override { AppRuntime::Shutdown(); }

  void Drain() { ASSERT_TRUE(AppRuntime::DrainWorkersThenUI()); }

  std::unique_ptr<DeliveryOutbox> MakeOutbox(FakeRelay& relay) {
    auto outbox = std::make_unique<DeliveryOutbox>([]() -> IDirectMessageClient* { return nullptr; });
    outbox->SetRelayClient(&relay);
    outbox->SetOnOutcome([this](const DeliveryOutcome& outcome) {
      std::lock_guard lock(mu_);
      outcomes_.push_back(outcome);
    });
    return outbox;
  }

  static DeliveryRequest ToRelay(const std::string& thread, const std::string& message) {
    DeliveryRequest request;
    request.envelope.message_id = message;
    request.envelope.timestamp = util::NowUnixMs();
    request.thread_id = thread;
    request.message_id = message;
    request.relay_route = "relay:bob";
    return request;
  }

  std::vector<DeliveryOutcome> Outcomes() {
    std::lock_guard lock(mu_);
    return outcomes_;
  }

  std::mutex mu_;
  std::vector<DeliveryOutcome> outcomes_;
};

TEST_F(DeliveryOutboxTest, ARelaySendReportsDelivered) {
  FakeRelay relay(0);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_TRUE(Outcomes()[0].ok);
  EXPECT_EQ(Outcomes()[0].transport, MessageTransport::Relay);
  EXPECT_FALSE(Outcomes()[0].relay_after_direct);
}

TEST_F(DeliveryOutboxTest, ARefusedSendIsReportedFailedThenDeliveredByARetry) {
  FakeRelay relay(1);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok) << "the row shows Failed while the retry is queued";
  outbox->Retry();
  Drain();
  ASSERT_EQ(Outcomes().size(), 2u);
  EXPECT_TRUE(Outcomes()[1].ok);
  EXPECT_EQ(relay.sent.size(), 2u);
}

TEST_F(DeliveryOutboxTest, CancelAndPurgeDropQueuedSends) {
  FakeRelay relay(2);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  outbox->Send(ToRelay("t2", "m2"));
  Drain();
  outbox->Cancel("m1");
  outbox->PurgeThread("t2");
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.sent.size(), 2u) << "nothing was sent again";
}

TEST_F(DeliveryOutboxTest, APausedThreadsRetriesAreDropped) {
  FakeRelay relay(1);
  auto outbox = MakeOutbox(relay);
  outbox->SetThreadPaused([](const std::string& thread_id) { return thread_id == "t1"; });
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.sent.size(), 1u);
}

TEST_F(DeliveryOutboxTest, NoRouteAndNoDirectTransportFailsAsNoAddress) {
  FakeRelay relay(0);
  auto outbox = MakeOutbox(relay);
  DeliveryRequest request = ToRelay("t1", "m1");
  request.relay_route.reset();
  request.amp_peer_key = "account:bob";
  outbox->Send(std::move(request));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_EQ(Outcomes()[0].failure, SendFailure::NoAddress);
  EXPECT_TRUE(relay.sent.empty());
}

} // namespace
} // namespace pbr
