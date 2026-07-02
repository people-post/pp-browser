#include "base/crypto/CryptoUtil.h"
#include "base/messaging/EnvelopeSigner.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/RelayWirePayload.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/people/Ed25519Signer.h"

#include <gtest/gtest.h>

TEST(RelayHistoryTest, MockFetchFiltersBySeqRange) {
  using namespace pbr;

  MockRelayClient relay;
  const auto test_private_key = HexToBytes(
      "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  ASSERT_TRUE(test_private_key);
  relay.SetReplySigningPrivateKey(*test_private_key);

  auto make_envelope = [&](uint64_t seq, const std::string& text) {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = "hist-" + std::to_string(seq);
    envelope.sender_relay_id = "relay:peer";
    envelope.sender_contact_id = "relay:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;
    auto payload = RelayWirePayload::EncodePlaintextText(text);
    EXPECT_TRUE(payload);
    envelope.body.e2e.payload_b64 = *payload;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(seq);
    auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
    EXPECT_TRUE(sign_bytes);
    auto signature =
        Ed25519Signer::Sign(std::string(sign_bytes->begin(), sign_bytes->end()), *test_private_key);
    EXPECT_TRUE(signature);
    envelope.signature = *signature;
    return envelope;
  };

  RelayEnvelope outbound = make_envelope(1, "seed");
  outbound.sender_contact_id = "relay:local";
  outbound.sender_relay_id = "relay:local";
  relay.SetNextReplySenderId("relay:peer");
  ASSERT_TRUE(static_cast<bool>(relay.Send(outbound)));

  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = "relay:local";
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = "relay:peer";
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.min_sender_seq = 2;
  request.max_sender_seq = 2;
  request.limit = 10;
  request.order = "asc";

  auto response = relay.FetchChatHistory(request);
  ASSERT_TRUE(response);
  ASSERT_EQ(response->messages.size(), 1u);
  EXPECT_EQ(response->messages.front().sender_seq, 2u);
}

TEST(RelayHistoryTest, HistoryRequestBuildsQueryString) {
  using namespace pbr;

  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = "relay:local";
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = "relay:peer";
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.min_sender_seq = 10;
  request.max_sender_seq = 42;
  request.limit = 50;
  request.order = "asc";

  const std::string query = ChatHistoryRequestToQueryString(request);
  EXPECT_NE(query.find("peer_identity_value=relay:peer"), std::string::npos);
  EXPECT_NE(query.find("min_sender_seq=10"), std::string::npos);
  EXPECT_NE(query.find("max_sender_seq=42"), std::string::npos);
}
