#include "lib/delivery/HttpRelayClient.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "common/tests/ScriptedHttpServer.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/OutboundMessage.h"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// The relay's message API by Account ID (multi-relay MR001 / MR003 / MR005): what a send carries, how the client meets
// the relay's refusals it can answer itself (our key, a grant it does not take), and how it reports the rest.

using test::ScriptedHttpServer;
using Reply = ScriptedHttpServer::Reply;

constexpr const char kMe[] = "account:me";
constexpr const char kBob[] = "account:bob";

RelayEnvelope EnvelopeTo(const std::string& recipient) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "11111111-2222-3333-4444-555555555555";
  envelope.sender_contact_id = kMe;
  envelope.sender_device = "QmMyPhone";
  envelope.route.kind = kRelayRouteDirect;
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.body.e2e.payload_b64 = "AA==";
  envelope.order_key = 7;
  envelope.stream_key = "v2:e2e_public:account:bob:account:me:from:QmMyPhone";
  envelope.recipient_account_id = recipient;
  envelope.signature = "envelope-sig";
  return envelope;
}

RelayGrant BobsGrant() {
  return RelayGrant{.recipient_account_id = kBob,
                    .sender_account_id = kMe,
                    .issued_at = 1761235200000,
                    .expires_at = 1761235200000 + kRelayGrantMaxLifetimeMs,
                    .signature = "Ym9icy1zaWduYXR1cmU="};
}

class HttpRelayClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_.SetAuthSigner([](const std::vector<uint8_t>&) -> Roe<std::string> { return std::string("c2ln"); });
    RelaySendHooks hooks;
    hooks.grant_for = [this](const std::string& recipient) -> std::optional<RelayGrant> {
      std::lock_guard lock(mu_);
      asked_grant_for_.push_back(recipient);
      return held_grant_;
    };
    hooks.grant_refused = [this](const std::string& recipient) {
      std::lock_guard lock(mu_);
      refused_.push_back(recipient);
    };
    hooks.sender_public_key = []() { return std::string("bXkta2V5"); };
    client_.SetSendHooks(std::move(hooks));
  }

  /** The relay answers the sends in order with `replies`, then 200. */
  void Script(std::vector<Reply> replies) {
    server_.SetHandler([this, replies = std::move(replies)](const ScriptedHttpServer::Request& request) {
      std::lock_guard lock(mu_);
      if (request.target == "/api/relay/v1/messages") {
        bodies_.push_back(TryParseObject(request.body).value_or(Object{}));
        if (bodies_.size() <= replies.size()) {
          return replies[bodies_.size() - 1];
        }
      }
      return Reply{.status = 200, .headers = {}, .body = R"({"success":true})"};
    });
  }

  std::vector<Object> Bodies() {
    std::lock_guard lock(mu_);
    return bodies_;
  }

  ScriptedHttpServer server_;
  HttpRelayClient client_{server_.Origin() + "/api/relay"};
  std::mutex mu_;
  std::vector<Object> bodies_;
  std::optional<RelayGrant> held_grant_;
  std::vector<std::string> asked_grant_for_;
  std::vector<std::string> refused_;
};

TEST_F(HttpRelayClientTest, ASendNamesBothAccountsAndCarriesTheRecipientsGrant) {
  held_grant_ = BobsGrant();
  Script({});
  ASSERT_TRUE(client_.Send(EnvelopeTo(kBob)));
  const auto bodies = Bodies();
  ASSERT_EQ(bodies.size(), 1u);
  EXPECT_EQ(bodies[0].getString("sender_account_id").value_or(""), kMe);
  EXPECT_EQ(bodies[0].getString("recipient_account_id").value_or(""), kBob);
  EXPECT_FALSE(bodies[0].contains("sender_contact_id"));
  EXPECT_FALSE(bodies[0].contains("sender_public_key_b64")) << "registered here: the relay holds our key";
  const Object* grant = bodies[0].getObject("grant");
  ASSERT_NE(grant, nullptr);
  EXPECT_EQ(RelayGrantFromJson(*grant).value(), BobsGrant());
}

TEST_F(HttpRelayClientTest, ASendToOurOwnMailboxAsksForNoGrant) {
  held_grant_ = BobsGrant();
  Script({});
  ASSERT_TRUE(client_.Send(EnvelopeTo(kMe)));
  EXPECT_TRUE(asked_grant_for_.empty());
  EXPECT_FALSE(Bodies()[0].contains("grant"));
}

TEST_F(HttpRelayClientTest, ARelayWithoutOurKeyGetsItOnTheNextTry) {
  Script({Reply{.status = 401, .headers = {}, .body = R"({"error":"x","code":"sender_key_required"})"}});
  ASSERT_TRUE(client_.Send(EnvelopeTo(kBob)));
  const auto bodies = Bodies();
  ASSERT_EQ(bodies.size(), 2u);
  EXPECT_FALSE(bodies[0].contains("sender_public_key_b64"));
  EXPECT_EQ(bodies[1].getString("sender_public_key_b64").value_or(""), "bXkta2V5");
}

TEST_F(HttpRelayClientTest, AGrantTheRelayRefusesIsReportedAndTheSendGoesOnceAsAStrangers) {
  for (const char* code : {"grant_expired", "invalid_grant"}) {
    held_grant_ = BobsGrant();
    bodies_.clear();
    refused_.clear();
    Script({Reply{.status = 401, .headers = {}, .body = std::string(R"({"error":"x","code":")") + code + R"("})"}});
    ASSERT_TRUE(client_.Send(EnvelopeTo(kBob))) << code;
    const auto bodies = Bodies();
    ASSERT_EQ(bodies.size(), 2u) << code;
    EXPECT_TRUE(bodies[0].contains("grant"));
    EXPECT_FALSE(bodies[1].contains("grant")) << code;
    EXPECT_EQ(refused_, std::vector<std::string>{kBob}) << "asked for a new one: " << code;
  }
}

TEST_F(HttpRelayClientTest, ARecipientNotRegisteredHereIsAFinalRefusalByItsCode) {
  Script({Reply{.status = 404, .headers = {}, .body = R"({"error":"x","code":"recipient_not_registered"})"}});
  auto sent = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_TRUE(RelayRecipientNotRegistered(sent.error()));
  EXPECT_FALSE(OutboxRetryAgain(1, sent.error())) << "final: not queued";
  EXPECT_EQ(Bodies().size(), 1u);
}

TEST_F(HttpRelayClientTest, APaymentRequiredCarriesTheRecipientsFloor) {
  Script({Reply{
      .status = 402, .headers = {}, .body = R"({"error":"x","code":"payment_required","initiation_floor":500})"}});
  auto sent = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_TRUE(RelayPaymentRequired(sent.error()));
  EXPECT_EQ(RelayPaymentRequiredFloor(sent.error()), 500);
  EXPECT_FALSE(OutboxRetryAgain(1, sent.error()));
}

TEST_F(HttpRelayClientTest, AStrangersQuotaIsWorthTryingAgainLater) {
  Script({Reply{.status = 429, .headers = {}, .body = R"({"error":"x","code":"stranger_quota_exceeded"})"},
          Reply{.status = 429, .headers = {}, .body = R"({"error":"x","code":"rate_limited"})"}});
  auto quota = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(quota);
  EXPECT_TRUE(RelayStrangerQuota(quota.error()));
  EXPECT_TRUE(OutboxRetryAgain(1, quota.error()));
  EXPECT_FALSE(OutboxRetryAgain(kMaxStrangerQuotaAttempts, quota.error()));
  auto limited = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(limited);
  EXPECT_FALSE(RelayStrangerQuota(limited.error()));
  EXPECT_EQ(static_cast<Err::Auth>(limited.error().code), Err::Auth::RateLimited);
}

TEST_F(HttpRelayClientTest, APollNamesTheRequesterByAccountAndReadsRecordsBySenderAccount) {
  RelayEnvelope from_bob = EnvelopeTo(kMe);
  from_bob.sender_contact_id = kBob;
  const std::string blob = DumpJson(RelayEnvelopeToApplicationJson(from_bob));
  Object record;
  record.set("sender_account_id", kBob);
  record.set("stream_id", "s1");
  record.set("index_key", static_cast<int64_t>(3));
  record.set("blob_b64", Base64Encode(ByteVector(blob.begin(), blob.end())));
  Object answer;
  answer.set("messages", ArrayValue({ObjectValue(record)}));
  answer.set("next_cursor", "c1");
  std::string poll_body;
  server_.SetHandler([&](const ScriptedHttpServer::Request& request) {
    std::lock_guard lock(mu_);
    poll_body = request.body;
    return Reply{.status = 200, .headers = {}, .body = DumpJson(answer)};
  });
  auto polled = client_.PollInbox(kMe, "");
  ASSERT_TRUE(polled) << polled.error().message;
  ASSERT_EQ(polled->messages.size(), 1u);
  EXPECT_EQ(polled->messages[0].sender_contact_id, kBob);
  std::lock_guard lock(mu_);
  auto sent = TryParseObject(poll_body).value();
  EXPECT_EQ(sent.getString("requester_account_id").value_or(""), kMe);
  EXPECT_FALSE(sent.contains("requester_contact_id"));
}

} // namespace
} // namespace pbr
