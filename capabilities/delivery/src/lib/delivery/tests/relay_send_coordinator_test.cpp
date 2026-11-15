#include "lib/delivery/RelaySendCoordinator.h"

#include "common/ValueJson.h"
#include "common/tests/ScriptedHttpServer.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/HttpRelayClient.h"
#include "lib/net/RelayOrigins.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// Sending in list order (multi-relay R2; docs/contracts/SERVICE_ENDPOINTS.md § Sending by the list): a contact's relays
// in its card's order, a relay that just failed after the healthy ones, our preferred relay when no list is known, and
// `recipient_not_registered` everywhere answered by the newest card and one more try.

constexpr const char kMe[] = "account:me";
constexpr const char kBob[] = "account:bob";
constexpr const char kOurs[] = "https://ours.example/api/relay";
constexpr const char kOurOther[] = "https://ours-too.example/api/relay";
constexpr const char kBobsA[] = "https://a.example/api/relay";
constexpr const char kBobsB[] = "https://b.example/api/relay";

Error Down() {
  return AppError::Network(Err::Network::ConnectFailed, "connect failed");
}
Error ServerError() {
  return AppError::Network(Err::Network::HttpError, "Relay send failed with status 503");
}
Error NotHere() {
  return AppError::Network(Err::Network::RecipientNotRegistered, "not registered here");
}
Error Price() {
  return AppError::Network(Err::Network::PaymentRequired, "initiation_floor=100");
}
Error Quota() {
  return AppError::Auth(Err::Auth::StrangerQuota, "quota spent");
}
Error WrongAudience() {
  return AppError::Auth(Err::Auth::WrongAudience, "Relay refused the command: signed for another relay");
}
Error IdentityChanged() {
  return AppError::Auth(Err::Auth::RelayIdentityChanged, "presented another identity");
}

/** The PeerId a card names for `origin` in these tests (MR007). */
std::string PeerIdOf(const std::string& origin) {
  return "Qm:" + RelayOriginKey(origin);
}

/** A card's relay list: each origin with the PeerId the card names for it. */
std::vector<AccountCardRelay> Card(const std::vector<std::string>& origins) {
  std::vector<AccountCardRelay> relays;
  for (const std::string& origin : origins) {
    relays.push_back(AccountCardRelay{.origin = origin, .peer_id = PeerIdOf(origin)});
  }
  return relays;
}

/** One relay: answers each send with the next scripted result (then success), and says who it is to a shared log. */
class FakeRelay : public IRelayClient {
public:
  FakeRelay(std::string origin, std::shared_ptr<std::vector<std::string>> log)
      : origin_(std::move(origin)), log_(std::move(log)) {}
  void Answer(std::vector<std::optional<Error>> answers) {
    std::lock_guard lock(mutex_);
    answers_.assign(answers.begin(), answers.end());
  }
  void AnswerAlways(Error error) {
    std::lock_guard lock(mutex_);
    always_ = std::move(error);
  }
  Roe<void> Send(const RelayEnvelope&) override {
    std::lock_guard lock(mutex_);
    log_->push_back(origin_);
    if (always_) {
      return *always_;
    }
    if (answers_.empty()) {
      return {};
    }
    std::optional<Error> next = answers_.front();
    answers_.pop_front();
    if (next) {
      return *next;
    }
    return {};
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override {
    polled_ = true;
    return RelayPollResult{};
  }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return RelayDeleteResult{}; }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return RelayDeleteResult{}; }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return ChatHistoryResponse{}; }

  bool polled_ = false;

private:
  std::string origin_;
  std::shared_ptr<std::vector<std::string>> log_;
  std::mutex mutex_;
  std::deque<std::optional<Error>> answers_;
  std::optional<Error> always_;
};

RelayEnvelope EnvelopeTo(const std::string& recipient) {
  RelayEnvelope envelope;
  envelope.message_id = "m1";
  envelope.sender_contact_id = kMe;
  envelope.recipient_account_id = recipient;
  return envelope;
}

class RelaySendCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    coordinator_.SetClock([this]() { return now_ms_; });
    coordinator_.SetOurRelays({{kOurs, Relay(kOurs)}, {kOurOther, Relay(kOurOther)}});
    coordinator_.SetForeignClients(
        [this](const std::string& origin, const std::string& peer_id) -> std::shared_ptr<IRelayClient> {
          made_.push_back(origin);
          made_for_.push_back(peer_id);
          return Relay(origin);
        });
    RelayRoutes routes;
    routes.relays_of = [this](const std::string& account) {
      return account == kBob ? Card(bobs_relays_) : std::vector<AccountCardRelay>{};
    };
    routes.refresh_relays_of = [this](const std::string& account) {
      refreshed_.push_back(account);
      bobs_relays_ = bobs_newest_relays_;
      return Card(bobs_relays_);
    };
    coordinator_.SetRoutes(std::move(routes));
  }

  std::shared_ptr<FakeRelay> Relay(const std::string& origin) {
    auto& relay = relays_[RelayOriginKey(origin)];
    if (!relay) {
      relay = std::make_shared<FakeRelay>(origin, log_);
    }
    return relay;
  }

  std::vector<std::string> Tried() {
    std::vector<std::string> tried = *log_;
    log_->clear();
    return tried;
  }

  int64_t now_ms_ = 1'000'000;
  std::shared_ptr<std::vector<std::string>> log_ = std::make_shared<std::vector<std::string>>();
  std::map<std::string, std::shared_ptr<FakeRelay>> relays_;
  std::vector<std::string> made_;
  /** The PeerId each client not ours was made to sign for. */
  std::vector<std::string> made_for_;
  std::vector<std::string> refreshed_;
  std::vector<std::string> bobs_relays_;
  std::vector<std::string> bobs_newest_relays_;
  RelaySendCoordinator coordinator_;
};

TEST_F(RelaySendCoordinatorTest, AContactWithoutAListIsSentThroughOurPreferredRelay) {
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kOurs})) << "today's way: our preferred relay alone";
  EXPECT_TRUE(made_.empty());
}

TEST_F(RelaySendCoordinatorTest, AContactsListIsTriedInOrderAndTheFirstThatTakesItEndsTheSend) {
  bobs_relays_ = {kBobsA, kBobsB};
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA})) << "the preferred of his list took it";
  EXPECT_EQ(made_, std::vector<std::string>({kBobsA})) << "a relay not ours gets a client of its own";
}

TEST_F(RelaySendCoordinatorTest, ATransportFailureOrA5xxGoesOnToTheNextRelay) {
  bobs_relays_ = {kBobsA, kBobsB};
  Relay(kBobsA)->Answer({Down()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA, kBobsB}));

  now_ms_ += RelayHealth::kRelayFailedForMs; // A has recovered by now
  Relay(kBobsA)->Answer({ServerError()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA, kBobsB}));
}

TEST_F(RelaySendCoordinatorTest, ARelayThatJustFailedIsTriedAfterTheHealthyOnesForABoundedTime) {
  bobs_relays_ = {kBobsA, kBobsB};
  Relay(kBobsA)->Answer({Down()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  Tried();

  now_ms_ += RelayHealth::kRelayFailedForMs - 1;
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsB})) << "A failed a moment ago: B first";

  now_ms_ += 1;
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA})) << "the memory is bounded: A is first again";
}

TEST_F(RelaySendCoordinatorTest, AFailedRelayIsStillTriedWhenTheHealthyOnesFail) {
  bobs_relays_ = {kBobsA, kBobsB};
  Relay(kBobsA)->Answer({Down()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  Tried();
  Relay(kBobsB)->Answer({Down()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsB, kBobsA}));
}

TEST_F(RelaySendCoordinatorTest, ARelaysAnswerAboutTheRecipientIsTheSendsAnswer) {
  bobs_relays_ = {kBobsA, kBobsB};
  for (const Error& answer : {Price(), Quota(), AppError::Network(Err::Network::HttpRejected, "400")}) {
    Relay(kBobsA)->Answer({answer});
    auto sent = coordinator_.Send(EnvelopeTo(kBob));
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().code, answer.code);
    EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA})) << answer.message << ": not a reason to try B";
    EXPECT_FALSE(coordinator_.Health().FailedRecently(kBobsA, now_ms_)) << "the relay itself is fine";
  }
}

TEST_F(RelaySendCoordinatorTest, EveryRelayFailingReportsTheFailureToRetryLater) {
  bobs_relays_ = {kBobsA, kBobsB};
  Relay(kBobsA)->AnswerAlways(NotHere());
  Relay(kBobsB)->AnswerAlways(Down());
  auto sent = coordinator_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_EQ(static_cast<Err::Network>(sent.error().code), Err::Network::ConnectFailed)
      << "B may hold his mailbox: the send is queued, not given up";
  EXPECT_TRUE(refreshed_.empty()) << "not every relay said he is not there";
}

TEST_F(RelaySendCoordinatorTest, NotRegisteredOnEveryRelayFetchesTheNewestCardAndTriesItsListOnce) {
  bobs_relays_ = {kBobsA};
  bobs_newest_relays_ = {kBobsB};
  Relay(kBobsA)->AnswerAlways(NotHere());
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(refreshed_, std::vector<std::string>({kBob}));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA, kBobsB}));
}

TEST_F(RelaySendCoordinatorTest, TheRetryAfterARefreshIsOnlyOnce) {
  bobs_relays_ = {kBobsA};
  bobs_newest_relays_ = {kBobsB};
  Relay(kBobsA)->AnswerAlways(NotHere());
  Relay(kBobsB)->AnswerAlways(NotHere());
  auto sent = coordinator_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_EQ(static_cast<Err::Network>(sent.error().code), Err::Network::RecipientNotRegistered);
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA, kBobsB}));
  EXPECT_EQ(refreshed_.size(), 1u);

  ASSERT_FALSE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(refreshed_.size(), 1u) << "asked again within the minute: no second fetch";
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsB}));

  now_ms_ += RelaySendCoordinator::kRefreshAgainAfterMs;
  ASSERT_FALSE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(refreshed_.size(), 2u);
}

// A message and its control-lane record go out together: the second send to find the list stale waits for the
// first's card fetch and takes its list, rather than giving up (the lab's users_on_two_relays_meet).
TEST_F(RelaySendCoordinatorTest, ASendWhileTheCardIsBeingFetchedWaitsForThatFetch) {
  std::mutex mu;
  std::condition_variable cv;
  bool fetching = false;
  bool release = false;
  RelayRoutes routes;
  routes.relays_of = [this, &mu](const std::string&) {
    std::lock_guard lock(mu);
    return Card(bobs_relays_);
  };
  routes.refresh_relays_of = [this, &mu, &cv, &fetching, &release](const std::string& account) {
    std::unique_lock lock(mu);
    refreshed_.push_back(account);
    fetching = true;
    cv.notify_all();
    cv.wait(lock, [&release] { return release; });
    bobs_relays_ = {kBobsB};
    return Card(bobs_relays_);
  };
  coordinator_.SetRoutes(std::move(routes));
  Relay(kOurs)->AnswerAlways(NotHere());
  Roe<void> first = Error("not sent");
  std::thread one([&] { first = coordinator_.Send(EnvelopeTo(kBob)); });
  {
    std::unique_lock lock(mu);
    cv.wait(lock, [&fetching] { return fetching; });
  }
  Roe<void> second = Error("not sent");
  std::thread two([&] { second = coordinator_.Send(EnvelopeTo(kBob)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50)); // the second is waiting for the fetch by now
  {
    std::lock_guard lock(mu);
    release = true;
  }
  cv.notify_all();
  one.join();
  two.join();
  EXPECT_TRUE(first);
  EXPECT_TRUE(second) << "it took the first send's fetched list";
  EXPECT_EQ(refreshed_.size(), 1u) << "one fetch for both";
}

TEST_F(RelaySendCoordinatorTest, NotRegisteredOnOurPreferredRelayFetchesTheCardOfAContactWithoutOne) {
  bobs_newest_relays_ = {kBobsB};
  Relay(kOurs)->AnswerAlways(NotHere());
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kOurs, kBobsB}));
}

TEST_F(RelaySendCoordinatorTest, ACopyForOurOtherDevicesGoesToOurRelaysInOrder) {
  Relay(kOurs)->Answer({Down()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kMe)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kOurs, kOurOther}));
  EXPECT_TRUE(made_.empty());
}

TEST_F(RelaySendCoordinatorTest, ARelayOfOursOnAContactsListIsSentAsOurs) {
  bobs_relays_ = {"https://OURS-TOO.example:443/api/relay/"};
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kOurOther}));
  EXPECT_TRUE(made_.empty()) << "the same relay, written differently: our registered client";
}

// MR007-SEND: a send into a contact's relay is signed for the PeerId its card names.
TEST_F(RelaySendCoordinatorTest, AContactsRelayIsSignedForTheCardsPeerId) {
  bobs_relays_ = {kBobsA};
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(made_for_, std::vector<std::string>({PeerIdOf(kBobsA)}));
}

// MR007-SEND: a card entry our pins disagree with is skipped — the next relay of the list takes the send — and the
// skipped relay is not marked unhealthy; the contact page hears of it.
TEST_F(RelaySendCoordinatorTest, ACardEntryConflictingWithAPinIsSkipped) {
  bobs_relays_ = {kBobsA, kBobsB};
  coordinator_.SetCardRelayCheck([](const std::string& origin, const std::string&) {
    return SameRelay(origin, kBobsA) ? CardRelayCheck::Mismatch : CardRelayCheck::Usable;
  });
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsB}));
  EXPECT_FALSE(coordinator_.Health().FailedRecently(kBobsA, now_ms_));
  EXPECT_TRUE(coordinator_.AudienceMismatch(kBob, kBobsA));
  EXPECT_FALSE(coordinator_.AudienceMismatch(kBob, kBobsB));
  EXPECT_TRUE(refreshed_.empty()) << "a pin conflict is not a stale list";

  bobs_relays_ = {kBobsA};
  auto sent = coordinator_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent) << "his only relay does not match";
  EXPECT_EQ(static_cast<Err::Auth>(sent.error().code), Err::Auth::WrongAudience);
}

// MR007-SEND: `wrong_audience` tries the next relay, counts like recipient_not_registered toward the newest-card
// fetch, and leaves RelayHealth alone.
TEST_F(RelaySendCoordinatorTest, AWrongAudienceTriesTheNextRelayWithoutAHealthMark) {
  bobs_relays_ = {kBobsA, kBobsB};
  Relay(kBobsA)->Answer({WrongAudience()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA, kBobsB}));
  EXPECT_FALSE(coordinator_.Health().FailedRecently(kBobsA, now_ms_));
  EXPECT_TRUE(coordinator_.AudienceMismatch(kBob, kBobsA));
}

TEST_F(RelaySendCoordinatorTest, AWrongAudienceEverywhereFetchesTheNewestCard) {
  bobs_relays_ = {kBobsA};
  bobs_newest_relays_ = {kBobsB}; // its owner re-issued for the relay's new identity, or moved
  Relay(kBobsA)->Answer({WrongAudience()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(refreshed_, std::vector<std::string>({kBob}));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA, kBobsB}));
}

TEST_F(RelaySendCoordinatorTest, ARefusedPeerIdIsSkippedForThatRecipientForFiveMinutes) {
  bobs_relays_ = {kBobsA, kBobsB};
  Relay(kBobsA)->Answer({WrongAudience()});
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  Tried();
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsB})) << "the newest card still names the refused PeerId";
  now_ms_ += RelaySendCoordinator::kAudienceRefusedMs;
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kBob)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kBobsA}));
  EXPECT_FALSE(coordinator_.AudienceMismatch(kBob, kBobsA));
}

// A relay of ours whose identity changed (not accepted) signs nothing: the send goes on, the relay is not unhealthy.
TEST_F(RelaySendCoordinatorTest, ARelayOfOursWhoseIdentityChangedIsSkipped) {
  Relay(kOurs)->AnswerAlways(IdentityChanged());
  ASSERT_TRUE(coordinator_.Send(EnvelopeTo(kMe)));
  EXPECT_EQ(Tried(), std::vector<std::string>({kOurs, kOurOther}));
  EXPECT_FALSE(coordinator_.Health().FailedRecently(kOurs, now_ms_));
}

TEST_F(RelaySendCoordinatorTest, TheInboxIsOurPreferredRelays) {
  ASSERT_TRUE(coordinator_.PollInbox(kMe, ""));
  EXPECT_TRUE(Relay(kOurs)->polled_);
  EXPECT_FALSE(Relay(kOurOther)->polled_);
}

TEST(RelaySendCoordinatorNoRelayTest, NoRelayFailsTheSend) {
  RelaySendCoordinator coordinator;
  auto sent = coordinator.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_EQ(AppError::CategoryOf(sent.error()), ErrorCategory::Config);
}

// Through the real client: Bob's relay is not one we registered on, so it asks for our key (MR003), and the send
// goes to it once more with the key.
TEST(RelaySendCoordinatorHttpTest, AForeignRelayGetsOurKeyWhenItAsksForIt) {
  using test::ScriptedHttpServer;
  ScriptedHttpServer bobs;
  std::mutex mu;
  std::vector<Object> bodies;
  bobs.SetHandler([&](const ScriptedHttpServer::Request& request) {
    std::lock_guard lock(mu);
    bodies.push_back(TryParseObject(request.body).value_or(Object{}));
    if (bodies.size() == 1) {
      return ScriptedHttpServer::Reply{.status = 401, .headers = {}, .body = R"({"code":"sender_key_required"})"};
    }
    return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = R"({"success":true})"};
  });
  const std::string bobs_origin = bobs.Origin() + "/api/relay";
  RelaySendCoordinator coordinator;
  auto signer = [](const std::vector<uint8_t>&) -> Roe<std::string> { return std::string("c2ln"); };
  coordinator.SetOurRelays({{"http://127.0.0.1:9/api/relay", std::make_shared<HttpRelayClient>("")}});
  coordinator.SetForeignClients([&](const std::string& origin, const std::string& peer_id) {
    auto client = std::make_shared<HttpRelayClient>(origin);
    client->SetAuthSigner(signer);
    client->SetAudience(std::make_shared<FixedRelayAudience>(peer_id));
    client->SetSendHooks(
        {.grant_for = {}, .grant_refused = {}, .sender_public_key = [] { return std::string("a2V5"); }});
    return client;
  });
  coordinator.SetRoutes(
      {.relays_of = [&](const std::string&) { return Card({bobs_origin}); }, .refresh_relays_of = {}});
  RelayEnvelope envelope = EnvelopeTo(kBob);
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "11111111-2222-3333-4444-555555555555";
  envelope.sender_device = "QmMyPhone";
  envelope.route.kind = kRelayRouteDirect;
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.body.e2e.payload_b64 = "AA==";
  envelope.stream_key = "v2:e2e_public:account:bob:account:me:from:QmMyPhone";
  envelope.signature = "envelope-sig";
  ASSERT_TRUE(coordinator.Send(envelope));
  std::lock_guard lock(mu);
  ASSERT_EQ(bodies.size(), 2u);
  EXPECT_FALSE(bodies[0].contains("sender_public_key_b64"));
  EXPECT_EQ(bodies[1].getString("sender_public_key_b64").value_or(""), "a2V5");
  EXPECT_EQ(bodies[1].getString("audience").value_or(""), PeerIdOf(bobs_origin)) << "the card's PeerId (MR007)";
}

} // namespace
} // namespace pbr
