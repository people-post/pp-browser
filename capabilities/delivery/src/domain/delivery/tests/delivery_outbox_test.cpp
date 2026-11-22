#include "lib/delivery/OutboundMessage.h"
#include "lib/delivery/RelayClients.h"
#include "domain/delivery/DeliveryOutbox.h"
#include "domain/delivery/DeliveryRowMark.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"

#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/runtime/AppRuntime.h"
#include "common/chat/MessagingLimits.h"
#include "foundation/error/AppError.h"
#include "lib/net/OrgBackendClients.h"
#include "common/Utilities.h"
#include "common/tests/TeardownLikeTheApp.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace pbr {
namespace {

/**
 * A relay that refuses the first `failures` sends (with `failure`: by default one that is retried), and every send to
 * a recipient in `down` (RecipientDown).
 */
class FakeRelay final : public IRelayClient {
public:
  explicit FakeRelay(int failures, Error failure = Error("relay unreachable"))
      : failure_(std::move(failure)), failures_(failures) {}
  Roe<void> Send(const RelayEnvelope& envelope) override {
    std::lock_guard lock(mu_);
    sent.push_back(envelope.message_id);
    recipients.push_back(envelope.recipient_account_id.value_or(""));
    if (envelope.recipient_account_id && down_.count(*envelope.recipient_account_id) != 0) {
      return failure_;
    }
    if (failures_ > 0) {
      --failures_;
      return failure_;
    }
    return {};
  }
  void RecipientDown(const std::string& recipient, const bool down) {
    std::lock_guard lock(mu_);
    if (down) {
      down_.insert(recipient);
    } else {
      down_.erase(recipient);
    }
  }
  size_t SentCount() {
    std::lock_guard lock(mu_);
    return sent.size();
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }

  std::vector<std::string> sent;
  /** Each send's recipient, in order. */
  std::vector<std::string> recipients;

private:
  std::mutex mu_;
  Error failure_;
  int failures_;
  std::set<std::string> down_;
};

/** A relay whose sends wait while it is held (an offline send waiting out its timeout), then fail or go through. */
class GatedRelay final : public IRelayClient {
public:
  Roe<void> Send(const RelayEnvelope& envelope) override {
    std::unique_lock lock(mu_);
    sent_.push_back(envelope.message_id);
    entered_.notify_all();
    released_.wait(lock, [this] { return !held_; });
    if (fail_) {
      return Error("relay unreachable");
    }
    return {};
  }
  void Hold() {
    std::lock_guard lock(mu_);
    held_ = true;
  }
  /** Lets the waiting sends (and every later one) finish: refused when `fail`. */
  void Release(const bool fail) {
    std::lock_guard lock(mu_);
    fail_ = fail;
    held_ = false;
    released_.notify_all();
  }
  void SetFail(const bool fail) {
    std::lock_guard lock(mu_);
    fail_ = fail;
  }
  /** Until `count` sends have started. */
  bool WaitForSends(const size_t count) {
    std::unique_lock lock(mu_);
    return entered_.wait_for(lock, std::chrono::seconds(5), [&] { return sent_.size() >= count; });
  }
  std::vector<std::string> Sent() {
    std::lock_guard lock(mu_);
    return sent_;
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }

private:
  std::mutex mu_;
  std::condition_variable entered_;
  std::condition_variable released_;
  std::vector<std::string> sent_;
  bool held_ = false;
  bool fail_ = true;
};

/** A direct-chat transport with a live link (or not) to the peer; records what it carried. */
class FakeDirect final : public IDirectMessageClient {
public:
  explicit FakeDirect(bool connected) : connected_(connected) {}
  bool IsPeerReachable(const std::string&) const override { return true; }
  bool IsPeerConnected(const std::string&) const override { return connected_; }
  void SendEnvelopeAsync(const std::string&, const RelayEnvelope& envelope,
                         std::function<void(DirectSendRoe)> on_done) override {
    {
      std::lock_guard lock(mu_);
      sent.push_back(envelope.message_id);
    }
    if (on_done) {
      on_done(DirectSendRoe());
    }
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

  /** Freed like the app frees it (before the runtime stops): it posts raw `this` onto the workers. */
  test::TestOwner<DeliveryOutbox> MakeOutbox(IRelayClient& relay) {
    test::TestOwner<DeliveryOutbox> outbox([]() -> IDirectMessageClient* { return nullptr; });
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
    request.relay_recipient = "account:bob";
    return request;
  }

  static DeliveryRequest Control(const std::string& message) {
    DeliveryRequest request;
    request.envelope.message_id = message;
    request.envelope.timestamp = util::NowUnixMs();
    request.message_id = message;
    request.relay_recipient = "account:bob";
    request.critical_lane = true;
    return request;
  }

  /** Until `done`, or false after `budget`: a retry's timer runs on the coordinator. */
  static bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
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
}

// #431 (a): call control to a peer with a live link goes over it at once as well as by the relay
// (an inbox can lag tens of seconds); without a live link, the relay alone as before.
TEST_F(DeliveryOutboxTest, CallControlAlsoGoesOverALiveLinkAtOnce) {
  for (const bool connected : {true, false}) {
    FakeRelay relay(0);
    FakeDirect direct(connected);
    test::TestOwner<DeliveryOutbox> outbox([&direct]() -> IDirectMessageClient* { return &direct; });
    outbox->SetRelayClient(&relay);
    DeliveryRequest control;
    control.envelope.message_id = "ctl-1";
    control.envelope.timestamp = util::NowUnixMs();
    control.message_id = "ctl-1";
    control.amp_peer_key = "account:bob";
    control.relay_recipient = "account:bob";
    control.critical_lane = true;
    outbox->Send(control);
    // On the send itself, not a drain marker: the critical lane has more than one thread, so the marker can run
    // while the send still does. The direct copy goes before the relay's.
    ASSERT_TRUE(WaitFor([&] { return relay.SentCount() == 1u; }, std::chrono::seconds(5)))
        << "the relay copy always goes, connected=" << connected;
    EXPECT_EQ(direct.Sent().size(), connected ? 1u : 0u) << "connected=" << connected;
  }
}

TEST_F(DeliveryOutboxTest, ARefusedSendIsReportedFailedThenDeliveredByARetry) {
  FakeRelay relay(1);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_TRUE(Outcomes()[0].retrying) << "queued for a retry: the row stays queued (B236)";
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

// Signaling is live: a control send the relay failed goes again within a second, not on the next
// inbox wake; the failure shows at once and the retry's delivery after it.
TEST_F(DeliveryOutboxTest, AFailedControlSendIsRetriedPromptlyWithoutAWake) {
  FakeRelay relay(1);
  auto outbox = MakeOutbox(relay);
  outbox->Send(Control("ctl-1"));
  ASSERT_TRUE(WaitFor([&] { return Outcomes().size() == 2u; }, std::chrono::seconds(5)));
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_TRUE(Outcomes()[1].ok);
  EXPECT_EQ(relay.SentCount(), 2u);
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.SentCount(), 2u) << "nothing was left for the wake";
}

// A call invite refused for the stranger quota waits on its retry timer; if the recipient's grant arrives meanwhile,
// it goes at once, before anything sent after it with the grant (its `call_ended`): the lab's redial storm rang the
// callee for a call already over when the invite's timer fired after the end.
TEST_F(DeliveryOutboxTest, AGrantSendsAQuotaHeldControlRetryAtOnceAndInOrder) {
  FakeRelay relay(2, AppError::Auth(Err::Auth::StrangerQuota, "stranger_quota_exceeded"));
  auto outbox = MakeOutbox(relay);
  outbox->Send(Control("invite"));
  ASSERT_TRUE(WaitFor([&] { return Outcomes().size() == 1u; }, std::chrono::seconds(2))); // refused first
  outbox->Send(Control("ringing"));
  ASSERT_TRUE(WaitFor([&] { return Outcomes().size() == 2u; }, std::chrono::seconds(2)));
  outbox->ReleaseHeldFor("account:carol");
  EXPECT_EQ(relay.SentCount(), 2u) << "another recipient's grant releases nothing";
  outbox->ReleaseHeldFor("account:bob");
  ASSERT_EQ(relay.SentCount(), 4u) << "both at once, not on their timers";
  EXPECT_EQ(relay.sent[2], "invite");
  EXPECT_EQ(relay.sent[3], "ringing");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_EQ(relay.SentCount(), 4u) << "their timers send nothing again";
}

TEST_F(DeliveryOutboxTest, CancelStopsAControlRetry) {
  FakeRelay relay(1);
  auto outbox = MakeOutbox(relay);
  outbox->Send(Control("ctl-1"));
  ASSERT_TRUE(WaitFor([&] { return Outcomes().size() == 1u; }, std::chrono::seconds(2)));
  outbox->Cancel("ctl-1");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_EQ(relay.SentCount(), 1u);
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
  request.relay_recipient.reset();
  request.amp_peer_key = "account:bob";
  outbox->Send(std::move(request));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_EQ(Outcomes()[0].failure, SendFailure::NoAddress);
  EXPECT_TRUE(relay.sent.empty());
}

// #210: while a row's send is under way (first try or a queued retry) the outbox says since when, so the chat shows a
// clock; once its last request ends — however it ends — it says nothing, so the chat shows the red mark or nothing.
// What is reported and when it is retried stays as above.

TEST_F(DeliveryOutboxTest, AFailedFirstSendStaysActiveUntilItsRetriesAreUsedUp) {
  FakeRelay relay(100);
  auto outbox = MakeOutbox(relay);
  const int64_t before = util::NowUnixMs();
  outbox->Send(ToRelay("t1", "m1"));
  const std::optional<int64_t> started = outbox->SendStartedMs("m1");
  ASSERT_TRUE(started) << "active from the Send call itself";
  EXPECT_GE(*started, before);
  EXPECT_LE(*started, util::NowUnixMs());
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_EQ(outbox->SendStartedMs("m1"), started) << "queued for a retry: still under way";
  for (int retry = 1; retry < kMaxOutboxRetryAttempts; ++retry) {
    outbox->Retry();
    Drain();
    EXPECT_EQ(outbox->SendStartedMs("m1"), started) << "retry " << retry << ": the first start stands";
  }
  EXPECT_EQ(Outcomes().size(), 1u) << "a retry queued again reports nothing (as before)";
  outbox->Retry();
  Drain();
  EXPECT_FALSE(outbox->SendStartedMs("m1")) << "the retries are used up";
  ASSERT_EQ(Outcomes().size(), 2u);
  EXPECT_FALSE(Outcomes()[1].ok);
  EXPECT_EQ(relay.SentCount(), static_cast<size_t>(1 + kMaxOutboxRetryAttempts));
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.SentCount(), static_cast<size_t>(1 + kMaxOutboxRetryAttempts)) << "nothing left to retry";
}

TEST_F(DeliveryOutboxTest, ASuccessfulRetryEndsTheActiveSend) {
  FakeRelay relay(1);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  EXPECT_TRUE(outbox->SendStartedMs("m1"));
  outbox->Retry();
  Drain();
  ASSERT_EQ(Outcomes().size(), 2u);
  EXPECT_TRUE(Outcomes()[1].ok);
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
}

TEST_F(DeliveryOutboxTest, ARefusedSendEndsItAtOnce) {
  FakeRelay relay(1, AppError::Network(Err::Network::HttpRejected, "400"));
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_FALSE(outbox->SendStartedMs("m1")) << "not queued: nothing is under way";
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.SentCount(), 1u);
}

// Multi-relay MR003: a recipient not registered on our relay is a final refusal, worded as such — no route to look
// up again (the Account ID is the address), nothing queued.
TEST_F(DeliveryOutboxTest, ARecipientNotOnTheRelayFailsAtOnceAndIsNotQueued) {
  FakeRelay relay(1, AppError::Network(Err::Network::RecipientNotRegistered, "recipient_not_registered"));
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_EQ(Outcomes()[0].failure, SendFailure::RecipientNotOnRelay);
  EXPECT_EQ(Outcomes()[0].relay_recipient, "account:bob");
  EXPECT_FALSE(outbox->SendStartedMs("m1")) << "final: nothing is queued";
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.SentCount(), 1u);
}

// A record the relay should carry to every device names the peer as its Amp fallback: when the peer is not on our
// relay, it goes over a link to it instead (a desktop reachable without the relay).
TEST_F(DeliveryOutboxTest, ARecipientNotOnTheRelayIsReachedOverALinkWhenOneCanBeHad) {
  FakeRelay relay(1, AppError::Network(Err::Network::RecipientNotRegistered, "recipient_not_registered"));
  FakeDirect direct(false);
  test::TestOwner<DeliveryOutbox> outbox([&direct]() -> IDirectMessageClient* { return &direct; });
  outbox->SetRelayClient(&relay);
  outbox->SetOnOutcome([this](const DeliveryOutcome& outcome) {
    std::lock_guard lock(mu_);
    outcomes_.push_back(outcome);
  });
  DeliveryRequest request = ToRelay("t1", "m1");
  request.amp_fallback_key = "account:bob";
  outbox->Send(std::move(request));
  ASSERT_TRUE(WaitFor([&] { return !Outcomes().empty(); }, std::chrono::seconds(5)));
  EXPECT_TRUE(Outcomes()[0].ok);
  EXPECT_EQ(Outcomes()[0].transport, MessageTransport::Direct);
  EXPECT_EQ(direct.Sent(), (std::vector<std::string>{"m1"}));
}

// MR005: a stranger's send to a recipient that charges is refused with the price; final, and the outcome carries the
// floor so the chat can show it (pricing P001).
TEST_F(DeliveryOutboxTest, APaymentRequiredIsFinalAndCarriesTheFloor) {
  FakeRelay relay(1, AppError::Network(Err::Network::PaymentRequired,
                                       "Relay send: the recipient charges strangers initiation_floor=250"));
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_EQ(Outcomes()[0].failure, SendFailure::PaymentRequired);
  EXPECT_EQ(Outcomes()[0].initiation_floor, 250);
  EXPECT_EQ(Outcomes()[0].relay_recipient, "account:bob");
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
}

// MR005: a stranger's quota spent is worth trying again — later: a retry before its backoff does not send it.
TEST_F(DeliveryOutboxTest, AStrangerQuotaRefusalWaitsOutItsBackoff) {
  FakeRelay relay(1, AppError::Auth(Err::Auth::StrangerQuota, "stranger_quota_exceeded"));
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_EQ(Outcomes()[0].failure, SendFailure::RelayRateLimited);
  EXPECT_TRUE(outbox->SendStartedMs("m1")) << "queued: still under way";
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.SentCount(), 1u) << "not again before its backoff";
  EXPECT_TRUE(outbox->SendStartedMs("m1"));
  outbox->ReleaseHeldFor("account:carol");
  Drain();
  EXPECT_EQ(relay.SentCount(), 1u) << "another recipient's grant releases nothing";
  outbox->ReleaseHeldFor("account:bob");
  Drain();
  EXPECT_EQ(relay.SentCount(), 2u) << "Bob's grant arrived: a contact's send now, at once";
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_EQ(StrangerQuotaRetryAfter(1), std::chrono::minutes(1));
  EXPECT_EQ(StrangerQuotaRetryAfter(3), std::chrono::minutes(4));
  EXPECT_EQ(StrangerQuotaRetryAfter(12), std::chrono::minutes(16));
}

TEST_F(DeliveryOutboxTest, ASendThatEndsAtOnceLeavesNothingActive) {
  FakeRelay relay(0);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "delivered"));
  DeliveryRequest no_route = ToRelay("t1", "no-route");
  no_route.relay_recipient.reset();
  outbox->Send(std::move(no_route));
  Drain();
  EXPECT_FALSE(outbox->SendStartedMs("delivered"));
  EXPECT_FALSE(outbox->SendStartedMs("no-route"));

  test::TestOwner<DeliveryOutbox> no_relay([]() -> IDirectMessageClient* { return nullptr; });
  no_relay->Send(ToRelay("t1", "no-relay"));
  Drain();
  EXPECT_FALSE(no_relay->SendStartedMs("no-relay"));
}

TEST_F(DeliveryOutboxTest, ADirectDeliveryEndsTheActiveSend) {
  FakeRelay relay(0);
  FakeDirect direct(false);
  test::TestOwner<DeliveryOutbox> outbox([&direct]() -> IDirectMessageClient* { return &direct; });
  outbox->SetRelayClient(&relay);
  DeliveryRequest request = ToRelay("t1", "m1");
  request.amp_peer_key = "account:bob";
  outbox->Send(std::move(request));
  ASSERT_TRUE(
      WaitFor([&] { return direct.Sent().size() == 1u && !outbox->SendStartedMs("m1"); }, std::chrono::seconds(5)));
  EXPECT_TRUE(relay.sent.empty()) << "it went over the link";
}

TEST_F(DeliveryOutboxTest, CancelAndPurgeEndTheActiveSend) {
  FakeRelay relay(2);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  outbox->Send(ToRelay("t2", "m2"));
  Drain();
  EXPECT_TRUE(outbox->SendStartedMs("m1"));
  EXPECT_TRUE(outbox->SendStartedMs("m2"));
  outbox->Cancel("m1");
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_TRUE(outbox->SendStartedMs("m2"));
  outbox->PurgeThread("t2");
  EXPECT_FALSE(outbox->SendStartedMs("m2"));
}

TEST_F(DeliveryOutboxTest, APausedThreadsRetriesEndTheActiveSend) {
  // Dropped when the retry starts.
  {
    FakeRelay relay(1);
    auto outbox = MakeOutbox(relay);
    outbox->SetThreadPaused([](const std::string& thread_id) { return thread_id == "t1"; });
    outbox->Send(ToRelay("t1", "m1"));
    Drain();
    EXPECT_TRUE(outbox->SendStartedMs("m1"));
    outbox->Retry();
    Drain();
    EXPECT_FALSE(outbox->SendStartedMs("m1"));
  }
  // Paused while its retry was on the way (the worker drops it).
  {
    FakeRelay relay(1);
    auto outbox = MakeOutbox(relay);
    auto asked = std::make_shared<std::atomic<int>>(0);
    outbox->SetThreadPaused([asked](const std::string&) { return asked->fetch_add(1) > 0; });
    outbox->Send(ToRelay("t1", "m1"));
    Drain();
    EXPECT_TRUE(outbox->SendStartedMs("m1"));
    outbox->Retry();
    Drain();
    EXPECT_GE(asked->load(), 2) << "asked when the retry started and again on the worker";
    EXPECT_FALSE(outbox->SendStartedMs("m1"));
    EXPECT_EQ(relay.SentCount(), 1u);
  }
}

TEST_F(DeliveryOutboxTest, AGroupMessageIsActiveUntilItsLastMemberRequestEnds) {
  FakeRelay relay(0);
  relay.RecipientDown("carol", true);
  auto outbox = MakeOutbox(relay);
  auto member = [](const std::string& who) {
    DeliveryRequest request = ToRelay("group-thread", "m1");
    request.group_member = "account:" + who;
    request.envelope.recipient_account_id = who;
    return request;
  };
  outbox->Send(member("bob"));
  const std::optional<int64_t> started = outbox->SendStartedMs("m1");
  ASSERT_TRUE(started);
  outbox->Send(member("carol"));
  Drain();
  EXPECT_EQ(outbox->SendStartedMs("m1"), started) << "bob's copy is out, carol's is queued: still under way";
  relay.RecipientDown("carol", false);
  outbox->Retry();
  Drain();
  EXPECT_FALSE(outbox->SendStartedMs("m1")) << "the last member's request ended";
  ASSERT_EQ(Outcomes().size(), 3u);
}

TEST_F(DeliveryOutboxTest, TheControlLaneIsNotTracked) {
  FakeRelay relay(0);
  auto outbox = MakeOutbox(relay);
  outbox->Send(Control("ctl-1"));
  EXPECT_FALSE(outbox->SendStartedMs("ctl-1"));
  ASSERT_TRUE(WaitFor([&] { return Outcomes().size() == 1u; }, std::chrono::seconds(5)));
  EXPECT_FALSE(outbox->SendStartedMs("ctl-1"));
}

// A cancelled message starts no new send attempt, wherever its request is when Cancel comes: queued, its first send
// on the way, or held by a Retry. Only the attempt already handed to the relay can still go out. Nothing more is
// reported for it, and the record of the cancel goes once its last request ends.

TEST_F(DeliveryOutboxTest, CancelWhileTheFirstSendIsOnTheWayStopsItsRetries) {
  GatedRelay relay;
  relay.Hold();
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  ASSERT_TRUE(relay.WaitForSends(1));
  outbox->Cancel("m1");
  EXPECT_EQ(outbox->CancelledCountForTest(), 1u) << "the attempt on the way is remembered as cancelled";
  relay.Release(/*fail=*/true);
  Drain();
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.Sent().size(), 1u) << "the failed send was not queued again";
  EXPECT_TRUE(Outcomes().empty()) << "a cancelled request reports nothing more";
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_EQ(outbox->CancelledCountForTest(), 0u) << "the record goes with the last request";
}

TEST_F(DeliveryOutboxTest, CancelReachesEveryGroupMemberCopyOnTheWay) {
  GatedRelay relay;
  relay.Hold();
  auto outbox = MakeOutbox(relay);
  for (const std::string who : {"bob", "carol"}) {
    DeliveryRequest request = ToRelay("group-thread", "m1");
    request.group_member = "account:" + who;
    outbox->Send(std::move(request));
  }
  ASSERT_TRUE(relay.WaitForSends(1));
  outbox->Cancel("m1");
  relay.Release(/*fail=*/true);
  Drain();
  const size_t first_round = relay.Sent().size();
  EXPECT_LE(first_round, 2u);
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.Sent().size(), first_round) << "no member's copy was sent again";
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_EQ(outbox->CancelledCountForTest(), 0u);
}

TEST_F(DeliveryOutboxTest, CancelOfTheItemARetryHoldsKeepsItOutOfTheQueue) {
  GatedRelay relay;
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  ASSERT_EQ(relay.Sent().size(), 1u);
  relay.Hold();
  outbox->Retry();
  ASSERT_TRUE(relay.WaitForSends(2));
  outbox->Cancel("m1");
  relay.Release(/*fail=*/true);
  Drain();
  relay.SetFail(false);
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.Sent().size(), 2u) << "the retry's failure was not put back";
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_EQ(outbox->CancelledCountForTest(), 0u);
}

TEST_F(DeliveryOutboxTest, CancelOfALaterItemInARetrySkipsItAndLeavesTheOthers) {
  GatedRelay relay;
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  outbox->Send(ToRelay("t1", "m2"));
  Drain();
  ASSERT_EQ(relay.Sent(), (std::vector<std::string>{"m1", "m2"}));
  relay.Hold();
  outbox->Retry();
  ASSERT_TRUE(relay.WaitForSends(3)); // m1's retry, m2 behind it
  outbox->Cancel("m2");
  relay.Release(/*fail=*/true);
  Drain();
  EXPECT_EQ(relay.Sent(), (std::vector<std::string>{"m1", "m2", "m1"})) << "m2 was not tried";
  relay.SetFail(false);
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.Sent(), (std::vector<std::string>{"m1", "m2", "m1", "m1"})) << "m1 goes on as before";
  ASSERT_FALSE(Outcomes().empty());
  EXPECT_TRUE(Outcomes().back().ok);
  EXPECT_EQ(Outcomes().back().message_id, "m1");
  EXPECT_FALSE(outbox->SendStartedMs("m2"));
  EXPECT_EQ(outbox->CancelledCountForTest(), 0u);
}

// As before: a cancel ends what was sent until then; a Send of the same id afterwards is a new request and goes.
TEST_F(DeliveryOutboxTest, ASendOfTheSameIdAfterACancelGoesAsUsual) {
  GatedRelay relay;
  relay.Hold();
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  ASSERT_TRUE(relay.WaitForSends(1));
  outbox->Cancel("m1");
  outbox->Send(ToRelay("t1", "m1"));
  relay.Release(/*fail=*/true);
  Drain();
  ASSERT_EQ(relay.Sent().size(), 2u) << "the new request was tried";
  ASSERT_EQ(Outcomes().size(), 1u) << "only the new request reports";
  EXPECT_FALSE(Outcomes()[0].ok);
  EXPECT_TRUE(outbox->SendStartedMs("m1")) << "the new request is queued for a retry";
  relay.SetFail(false);
  outbox->Retry();
  Drain();
  EXPECT_EQ(relay.Sent().size(), 3u) << "only the new request was retried";
  ASSERT_EQ(Outcomes().size(), 2u);
  EXPECT_TRUE(Outcomes()[1].ok);
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_EQ(outbox->CancelledCountForTest(), 0u);
}

TEST_F(DeliveryOutboxTest, CancelOfAMessageWithNothingUnderWayRemembersNothing) {
  FakeRelay relay(0);
  auto outbox = MakeOutbox(relay);
  outbox->Send(ToRelay("t1", "m1"));
  Drain();
  outbox->Cancel("m1");
  outbox->Cancel("never-sent");
  EXPECT_EQ(outbox->CancelledCountForTest(), 0u);
}

// B236: a message shows "queued" while the outbox still tries and "failed" only once it gives up. The row is marked
// as the app marks it (ApplyOutcomeToRow, from the outcome handler, the outbox asked whether anything of the message is
// still under way), and every state it takes is recorded.

class OutboxRowTest : public DeliveryOutboxTest {
protected:
  void SetUp() override {
    DeliveryOutboxTest::SetUp();
    DirectChatTarget target{.peer_identity_kind = "account", .peer_identity_value = "account:bob"};
    auto thread = store_.FindOrCreateDirectThread(target, "", "Bob");
    ASSERT_TRUE(thread);
    thread_id_ = thread->id;
    ThreadMessage row;
    row.id = "m1";
    row.thread_id = thread_id_;
    row.sender_contact_id = kLocalSelfContactId;
    row.content_type = ChatContentType::Attachment;
    row.text = "photo.jpg";
    row.timestamp = util::NowUnixMs();
    row.delivery = MessageDelivery::Pending;
    row.relay_visible = true;
    ASSERT_TRUE(store_.AppendMessage(row));
  }

  test::TestOwner<DeliveryOutbox> MakeRowOutbox(IRelayClient& relay) {
    test::TestOwner<DeliveryOutbox> outbox([]() -> IDirectMessageClient* { return nullptr; });
    outbox->SetRelayClient(&relay);
    DeliveryOutbox* raw = &*outbox;
    outbox->SetOnOutcome([this, raw](const DeliveryOutcome& outcome) {
      const RowMarkResult marked =
          ApplyOutcomeToRow(store_, outcome, raw->SendStartedMs(outcome.message_id).has_value());
      std::lock_guard lock(mu_);
      outcomes_.push_back(outcome);
      if (marked.failed) {
        failed_reasons_.push_back(outcome.failure);
      }
      states_.push_back(Delivery());
    });
    return outbox;
  }

  DeliveryRequest Request(const std::string& copy_device = {}) const {
    DeliveryRequest request = ToRelay(thread_id_, "m1");
    request.copy_device = copy_device;
    return request;
  }

  MessageDelivery Delivery() const {
    auto row = store_.GetMessage(thread_id_, "m1");
    return row && *row ? (*row)->delivery : MessageDelivery::Local;
  }

  std::vector<MessageDelivery> States() {
    std::lock_guard lock(mu_);
    return states_;
  }

  std::vector<SendFailure> FailedReasons() {
    std::lock_guard lock(mu_);
    return failed_reasons_;
  }

  static bool EverFailed(const std::vector<MessageDelivery>& states) {
    return std::find(states.begin(), states.end(), MessageDelivery::Failed) != states.end();
  }

  InMemoryDeliveryStore store_;
  std::string thread_id_;
  std::vector<MessageDelivery> states_;
  std::vector<SendFailure> failed_reasons_;
};

TEST_F(OutboxRowTest, AStrangerQuotaRefusalStaysQueuedUntilARetryDeliversIt) {
  FakeRelay relay(1, AppError::Auth(Err::Auth::StrangerQuota, "stranger_quota_exceeded"));
  auto outbox = MakeRowOutbox(relay);
  outbox->Send(Request("relay"));
  Drain();
  ASSERT_EQ(Outcomes().size(), 1u);
  EXPECT_TRUE(Outcomes()[0].retrying);
  EXPECT_EQ(Delivery(), MessageDelivery::Pending) << "the 429 is not the end: queued, not failed";
  EXPECT_TRUE(outbox->SendStartedMs("m1")) << "the chat shows the clock (under way)";
  outbox->ReleaseHeldFor("account:bob"); // its backoff over (here: the grant), the retry goes
  Drain();
  EXPECT_EQ(Delivery(), MessageDelivery::Relayed);
  EXPECT_FALSE(EverFailed(States())) << "never shown failed on the way";
  EXPECT_TRUE(FailedReasons().empty()) << "nothing told";
}

TEST_F(OutboxRowTest, AStrangerQuotaUntilTheRetriesAreUsedUpFailsTheRowOnceWithItsReason) {
  FakeRelay relay(1000, AppError::Auth(Err::Auth::StrangerQuota, "stranger_quota_exceeded"));
  auto outbox = MakeRowOutbox(relay);
  outbox->Send(Request());
  Drain();
  for (int backoff = 0; backoff < 20 && outbox->SendStartedMs("m1"); ++backoff) {
    EXPECT_EQ(Delivery(), MessageDelivery::Pending) << "after backoff " << backoff << ": still queued";
    outbox->ReleaseHeldFor("account:bob"); // each backoff over
    Drain();
  }
  EXPECT_GE(relay.SentCount(), static_cast<size_t>(kMaxStrangerQuotaAttempts));
  EXPECT_EQ(Delivery(), MessageDelivery::Failed) << "the outbox gave up";
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_EQ(FailedReasons(), (std::vector<SendFailure>{SendFailure::RelayRateLimited})) << "failed once, with why";
  const std::vector<MessageDelivery> states = States();
  EXPECT_EQ(std::count(states.begin(), states.end(), MessageDelivery::Failed), 1);
  EXPECT_EQ(states.back(), MessageDelivery::Failed);
}

TEST_F(OutboxRowTest, ARelayThatDoesNotAnswerStaysQueuedThroughItsRetries) {
  FakeRelay relay(1000); // a network error: retried on each inbox sync
  auto outbox = MakeRowOutbox(relay);
  outbox->Send(Request());
  Drain();
  for (int retry = 1; retry <= kMaxOutboxRetryAttempts; ++retry) {
    EXPECT_EQ(Delivery(), MessageDelivery::Pending) << "before retry " << retry;
    outbox->Retry();
    Drain();
  }
  EXPECT_FALSE(outbox->SendStartedMs("m1"));
  EXPECT_EQ(Delivery(), MessageDelivery::Failed);
  EXPECT_EQ(FailedReasons().size(), 1u);
}

TEST_F(OutboxRowTest, AFinalRefusalFailsAtOnce) {
  FakeRelay relay(1, AppError::Network(Err::Network::HttpRejected, "400"));
  auto outbox = MakeRowOutbox(relay);
  outbox->Send(Request());
  Drain();
  EXPECT_EQ(Delivery(), MessageDelivery::Failed) << "refused for good: nothing more is tried";
  EXPECT_EQ(FailedReasons().size(), 1u);
}

TEST_F(OutboxRowTest, AQueuedSendDroppedForAPausedChatFailsWithItsLastReason) {
  FakeRelay relay(1, AppError::Auth(Err::Auth::StrangerQuota, "stranger_quota_exceeded"));
  auto outbox = MakeRowOutbox(relay);
  outbox->Send(Request());
  Drain();
  EXPECT_EQ(Delivery(), MessageDelivery::Pending);
  outbox->SetThreadPaused([](const std::string&) { return true; });
  outbox->ReleaseHeldFor("account:bob");
  Drain();
  EXPECT_EQ(Delivery(), MessageDelivery::Failed) << "dropped: not left queued forever";
  EXPECT_EQ(FailedReasons(), (std::vector<SendFailure>{SendFailure::RelayRateLimited}));
}

// M022: a message goes as several records (a device's copy over its link, the relay record for the rest). One ending
// in failure while another is still tried leaves it queued; the last one decides.
TEST_F(OutboxRowTest, OneCopyFailingWhileAnotherIsTriedLeavesTheRowQueued) {
  for (const bool second_gets_through : {false, true}) {
    ASSERT_TRUE(store_.UpdateMessageDelivery(thread_id_, "m1", MessageDelivery::Pending, std::nullopt, false));
    {
      std::lock_guard lock(mu_);
      states_.clear();
      failed_reasons_.clear();
    }
    FakeRelay relay(1000); // the relay record: refused (network) on every try until it is let through
    auto outbox = MakeRowOutbox(relay);
    DeliveryRequest no_route = Request("device-1"); // a copy that fails for good at once
    no_route.relay_recipient.reset();
    outbox->Send(Request("relay"));
    outbox->Send(std::move(no_route));
    Drain();
    EXPECT_EQ(Delivery(), MessageDelivery::Pending) << "one copy failed, the other is still tried";
    EXPECT_TRUE(FailedReasons().empty());
    FakeRelay through(0);
    for (int retry = 1; retry <= kMaxOutboxRetryAttempts; ++retry) {
      if (second_gets_through && retry == kMaxOutboxRetryAttempts) {
        outbox->SetRelayClient(&through);
      }
      outbox->Retry();
      Drain();
    }
    if (second_gets_through) {
      EXPECT_EQ(Delivery(), MessageDelivery::Relayed);
      EXPECT_FALSE(EverFailed(States()));
      EXPECT_TRUE(FailedReasons().empty());
    } else {
      EXPECT_EQ(Delivery(), MessageDelivery::Failed);
      EXPECT_EQ(FailedReasons().size(), 1u) << "told once, not per copy";
    }
  }
}

} // namespace
} // namespace pbr
