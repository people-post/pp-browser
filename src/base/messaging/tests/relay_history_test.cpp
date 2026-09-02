#include "foundation/crypto/CryptoUtil.h"
#include "base/messaging/EnvelopeSigner.h"
#include "common/chat/MessagingJson.h"
#include "common/chat/RelayStreamKey.h"
#include "base/messaging/RelayWirePayload.h"
#include "domain/net/ServiceClientsImpl.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>

TEST(RelayHistoryTest, MockFetchFiltersBySeqRange) {
  using namespace pbr;

  MockRelayClient relay;
  auto keys = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(keys));
  relay.SetReplySigningPrivateKey(keys->secret_key);
  relay.SetBuildSignBytesFn([](const RelayEnvelope& envelope) {
    return EnvelopeSigner::BuildSignBytes(envelope);
  });

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
    envelope.order_key = seq;
    envelope.session_epoch = 1;
    envelope.stream_key =
        BuildCanonicalRelayStreamKey("relay:local", "relay:peer", ThreadChannel::E2e, envelope.session_epoch);
    envelope.recipient_contact_id = "relay:local";
    envelope.timestamp = static_cast<int64_t>(seq);
    auto sign_bytes = EnvelopeSigner::BuildSignBytes(envelope);
    EXPECT_TRUE(sign_bytes);
    auto signature = MlDsa::Sign(keys->secret_key, *sign_bytes);
    EXPECT_TRUE(signature);
    envelope.signature = Base64Encode(*signature);
    return envelope;
  };

  RelayEnvelope outbound = make_envelope(1, "seed");
  outbound.sender_contact_id = "relay:local";
  outbound.sender_relay_id = "relay:local";
  outbound.recipient_contact_id = "relay:peer";
  outbound.stream_key =
      BuildCanonicalRelayStreamKey("relay:local", "relay:peer", ThreadChannel::E2e, outbound.session_epoch);
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
  EXPECT_NE(query.find("sender_contact_id=relay:peer"), std::string::npos);
  EXPECT_NE(query.find("stream_id="), std::string::npos);
  EXPECT_NE(query.find("min_index_key=10"), std::string::npos);
  EXPECT_NE(query.find("max_index_key=42"), std::string::npos);
}
