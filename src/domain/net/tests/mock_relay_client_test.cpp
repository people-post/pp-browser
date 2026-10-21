// The mock relay client the messaging tests lean on: what it answers for a history request.
#include "common/chat/RelayStreamKey.h"
#include "domain/net/testing/MockOrgBackendClients.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

TEST(MockRelayClientTest, FetchFiltersBySeqRange) {
  MockRelayClient relay;
  auto make_envelope = [&](uint64_t seq) {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = "hist-" + std::to_string(seq);
    envelope.sender_relay_id = "relay:peer";
    envelope.sender_contact_id = "relay:peer";
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;
    envelope.body.e2e.payload_b64 = "AQID";
    envelope.sender_seq = seq;
    envelope.order_key = seq;
    envelope.session_epoch = 1;
    envelope.stream_key =
        BuildCanonicalRelayStreamKey("relay:local", "relay:peer", ThreadChannel::E2e, envelope.session_epoch);
    envelope.recipient_contact_id = "relay:local";
    envelope.timestamp = static_cast<int64_t>(seq);
    return envelope;
  };
  for (uint64_t seq = 1; seq <= 3; ++seq) {
    relay.AddDeliveredEnvelope(make_envelope(seq));
  }

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

} // namespace
} // namespace pbr
