#include "lib/delivery/RelayClients.h"
#include "domain/delivery/DeliveryOutbox.h"

#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/runtime/AppRuntime.h"
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

/** A direct-chat transport with a live link (or not) to the peer; records what it carried. */
class FakeDirect final : public IDirectMessageClient {
public:
  explicit FakeDirect(bool connected) : connected_(connected) {}
  bool IsPeerReachable(const std::string&) const override { return true; }
  bool IsPeerConnected(const std::string&) const override { return connected_; }
  DirectSendRoe SendEnvelope(const std::string&, const RelayEnvelope& envelope) override {
    std::lock_guard lock(mu_);
    sent.push_back(envelope.message_id);
    return {};
  }
  std::vector<std::string> Sent() {
    std::lock_guard lock(mu_);
    return sent;
  }

private:
  bool connected_;
  std::mutex mu_;
  std::vector<std::string> sent;
};

class DeliveryOutboxTest : public ::testing::Test {
protected:
  void SetUp() override { AppRuntime::Initialize(AppRuntimeConfig{}); }
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

// #431 (a): call control to a peer with a live link goes over it at once as well as by the relay
// (an inbox can lag tens of seconds); without a live link, the relay alone as before.
TEST_F(DeliveryOutboxTest, CallControlAlsoGoesOverALiveLinkAtOnce) {
  for (const bool connected : {true, false}) {
    FakeRelay relay(0);
    FakeDirect direct(connected);
    DeliveryOutbox outbox([&direct]() -> IDirectMessageClient* { return &direct; });
    outbox.SetRelayClient(&relay);
    DeliveryRequest control;
    control.envelope.message_id = "ctl-1";
    control.envelope.timestamp = util::NowUnixMs();
    control.message_id = "ctl-1";
    control.amp_peer_key = "account:bob";
    control.relay_route = "relay:bob";
    control.critical_lane = true;
    outbox.Send(control);
    Drain();
    EXPECT_EQ(relay.sent.size(), 1u) << "the relay copy always goes, connected=" << connected;
    EXPECT_EQ(direct.Sent().size(), connected ? 1u : 0u) << "connected=" << connected;
  }
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

TEST_F(DeliveryOutboxTest, AGroupMembersCopyReportsForThatMemberThroughItsRetry) {
  FakeRelay relay(1);
  auto outbox = MakeOutbox(relay);
  DeliveryRequest request = ToRelay("group-thread", "m1");
  request.group_member = "account:carol";
  outbox->Send(std::move(request));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_EQ(Outcomes()[0].group_member, "account:carol") << "the member missed, not the message";
  outbox->Retry();
  Drain();
  ASSERT_EQ(Outcomes().size(), 2u);
  EXPECT_TRUE(Outcomes()[1].ok);
  EXPECT_EQ(Outcomes()[1].group_member, "account:carol");
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
