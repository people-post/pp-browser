#include "lib/delivery/HttpRelayClient.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "common/tests/ScriptedHttpServer.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/testing/TypedBlobFakes.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/OutboundMessage.h"
#include "lib/delivery/RelayGrantSigner.h"
#include "lib/delivery/RelayMessageSignPayload.h"

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
/** The relay's PeerId (MR007): every call is signed for it. */
constexpr const char kAudience[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";

/** A pinned audience that records what the relay refused. */
class RecordingAudience : public IRelayAudience {
public:
  Roe<std::string> PeerId() override {
    if (changed) {
      return AppError::Auth(Err::Auth::RelayIdentityMismatch, "presented another identity");
    }
    return std::string(kAudience);
  }
  void Refused(const std::string& code) override { refused.push_back(code); }
  bool changed = false;
  std::vector<std::string> refused;
};

RelayEnvelope EnvelopeTo(const std::string& recipient) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.message_id = "11111111-2222-3333-4444-555555555555";
  envelope.sender_contact_id = kMe;
  envelope.sender_device = "QmMyPhone";
  envelope.route.kind = kRelayRouteDirect;
  envelope.route.channel = ThreadChannel::E2ePublic;
  envelope.body.e2e.payload_b64 = test::FakePayloadB64();
  envelope.order_key = 7;
  envelope.stream_key = "v2:e2e_public:account:bob:account:me:from:QmMyPhone";
  envelope.recipient_account_id = recipient;
  envelope.signature = test::FakeSignatureB64();
  return envelope;
}

RelayGrant BobsGrant() {
  return RelayGrant{.recipient_account_id = kBob,
                    .sender_account_id = kMe,
                    .issued_at = 1761235200000,
                    .expires_at = 1761235200000 + kRelayGrantMaxLifetimeMs,
                    .signature = pp::Base64Encode(pp::ByteVector(3310, 0x01))}; // a typed ML-DSA-65 signature's shape
}

class HttpRelayClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_.SetAuthSigner([this](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
      std::lock_guard lock(mu_);
      signed_.push_back(bytes);
      return std::string("c2ln");
    });
    client_.SetAudience(audience_);
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
  std::vector<std::vector<uint8_t>> signed_;
  std::shared_ptr<RecordingAudience> audience_ = std::make_shared<RecordingAudience>();
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
  EXPECT_EQ(grant->getIf<int64_t>("grant_version"), 2) << "grant v2 (SIGNED_FORMATS § 2.4)";
  EXPECT_EQ(RelayGrantFromJson(*grant).value(), BobsGrant());
}

// MR007-AUD: a send names the relay it is for, in its body and in the bytes signed.
TEST_F(HttpRelayClientTest, ASendNamesItsRelayInBodyAndBytes) {
  Script({});
  ASSERT_TRUE(client_.Send(EnvelopeTo(kBob)));
  const auto bodies = Bodies();
  ASSERT_EQ(bodies.size(), 1u);
  EXPECT_EQ(bodies[0].getString("audience").value_or(""), kAudience);
  EXPECT_EQ(bodies[0].getIf<int64_t>("sign_version"), 3) << "the version byte its bytes carry";
  RelayWireSendRecord record;
  record.sender_account_id = kMe;
  record.recipient_account_id = kBob;
  record.stream_id = bodies[0].getString("stream_id").value_or("");
  record.index_key = bodies[0].getNonNegInt("index_key").value_or(0);
  const int64_t timestamp = bodies[0].getIf<int64_t>("timestamp").value_or(0);
  std::lock_guard lock(mu_);
  ASSERT_EQ(signed_.size(), 1u);
  EXPECT_EQ(signed_[0], BuildRelayApiSendSignBytes(kAudience, record, timestamp));
}

// MR007-AUD / MR007-PIN: `wrong_audience` is its own refusal, and the audience hears it (its identity is asked again).
TEST_F(HttpRelayClientTest, AWrongAudienceIsReportedToTheAudience) {
  Script(
      {Reply{.status = 401, .headers = {}, .body = R"({"error":"Signed for another relay","code":"wrong_audience"})"}});
  auto sent = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_EQ(AppError::CategoryOf(sent.error()), ErrorCategory::Auth);
  EXPECT_EQ(static_cast<Err::Auth>(sent.error().code), Err::Auth::WrongAudience);
  EXPECT_EQ(audience_->refused, std::vector<std::string>{"wrong_audience"});
  EXPECT_EQ(Bodies().size(), 1u) << "not sent again as it is";
}

// A relay newer than this app answers 426 `client_upgrade_required`: its own typed refusal, heard by the audience (the
// org's relay: the update screen; another of ours: its row), and not sent again.
TEST_F(HttpRelayClientTest, AnUpgradeRequiredIsReportedToTheAudience) {
  Script(
      {Reply{.status = 426,
             .headers = {},
             .body = R"({"error":"This app is too old for this relay; update it","code":"client_upgrade_required"})"}});
  auto sent = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_TRUE(IsClientUpgradeRequired(sent.error()));
  EXPECT_EQ(audience_->refused, std::vector<std::string>{"client_upgrade_required"});
  EXPECT_EQ(Bodies().size(), 1u) << "not sent again as it is";
  server_.SetHandler([](const ScriptedHttpServer::Request&) {
    return Reply{.status = 426, .headers = {}, .body = R"({"error":"x","code":"client_upgrade_required"})"};
  });
  auto polled = client_.PollInbox(kMe, "");
  ASSERT_FALSE(polled);
  EXPECT_TRUE(IsClientUpgradeRequired(polled.error()));
  server_.SetHandler([](const ScriptedHttpServer::Request&) {
    return Reply{.status = 426, .headers = {}, .body = R"({"error":"Upgrade Required"})"};
  });
  auto uncoded = client_.PollInbox(kMe, "");
  ASSERT_FALSE(uncoded);
  EXPECT_FALSE(IsClientUpgradeRequired(uncoded.error())) << "only the relay's code says so";
}

// U10/U11: a relay older than this app (a newer sign version than it reads) says so with 400
// `relay_upgrade_required`, or, before that code, `unsupported_sign_version`: typed as "the relay needs an update", not
// "update the app", heard by the audience under the one code, and final for a send.
TEST_F(HttpRelayClientTest, ARelayOlderThanThisAppIsTypedApartFromAnAppTooOld) {
  for (const char* code : {"relay_upgrade_required", "unsupported_sign_version"}) {
    audience_->refused.clear();
    const std::string body = std::string(R"({"error":"x","code":")") + code + R"("})";
    server_.SetHandler(
        [body](const ScriptedHttpServer::Request&) { return Reply{.status = 400, .headers = {}, .body = body}; });
    auto polled = client_.PollInbox(kMe, "");
    ASSERT_FALSE(polled);
    EXPECT_TRUE(IsRelayUpgradeRequired(polled.error())) << code;
    EXPECT_FALSE(IsClientUpgradeRequired(polled.error())) << code;
    EXPECT_TRUE(IsPermanentRelayRefusal(polled.error())) << code;
    EXPECT_EQ(audience_->refused, std::vector<std::string>{"relay_upgrade_required"}) << code;
  }
  // A send into such a relay is the relay's refusal: not sent again as it is.
  Script({Reply{.status = 400, .headers = {}, .body = R"({"error":"x","code":"relay_upgrade_required"})"}});
  auto sent = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_TRUE(IsRelayUpgradeRequired(sent.error()));
  EXPECT_EQ(Bodies().size(), 1u) << "not sent again as it is";
  EXPECT_FALSE(OutboxRetryAgain(1, sent.error())) << "final, as the 400 was before it was typed";
}

// MR007-HEAL: a poll answered not_registered reports the lost registration.
TEST_F(HttpRelayClientTest, APollAnsweredNotRegisteredIsReportedToTheAudience) {
  server_.SetHandler([](const ScriptedHttpServer::Request&) {
    return Reply{.status = 403, .headers = {}, .body = R"({"error":"Account not registered","code":"not_registered"})"};
  });
  auto polled = client_.PollInbox(kMe, "");
  ASSERT_FALSE(polled);
  EXPECT_EQ(static_cast<Err::Auth>(polled.error().code), Err::Auth::NotRegistered);
  EXPECT_EQ(audience_->refused, std::vector<std::string>{"not_registered"});
}

// MR007-PIN: no call is signed for a relay whose identity changed.
TEST_F(HttpRelayClientTest, NothingGoesToARelayWhoseIdentityChanged) {
  audience_->changed = true;
  Script({});
  auto sent = client_.Send(EnvelopeTo(kBob));
  ASSERT_FALSE(sent);
  EXPECT_EQ(static_cast<Err::Auth>(sent.error().code), Err::Auth::RelayIdentityMismatch);
  EXPECT_FALSE(client_.PollInbox(kMe, ""));
  EXPECT_TRUE(Bodies().empty());
  std::lock_guard lock(mu_);
  EXPECT_TRUE(signed_.empty());
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
  EXPECT_EQ(sent.getString("audience").value_or(""), kAudience);
}

} // namespace
} // namespace pbr
