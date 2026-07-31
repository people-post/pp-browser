#include "base/messaging/SfuAttachFanout.h"
#include "base/messaging/CallControlCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(SfuAttachFanoutTest, ClearsConsumedQuoteId) {
  CallSfuAttachDetail after;
  after.call_id = "call:1";
  after.hop_peer_id = "12D3KooWHop";
  after.hop_multiaddr = "/ip4/1.2.3.4/tcp/4001/p2p/12D3KooWHop";
  after.quote_id = "quote-consumed";
  after.publisher_stream_id = 42;

  const CallSfuAttachDetail fanout = BuildSfuAttachFanout(after);
  EXPECT_EQ(fanout.call_id, after.call_id);
  EXPECT_EQ(fanout.hop_peer_id, after.hop_peer_id);
  EXPECT_EQ(fanout.hop_multiaddr, after.hop_multiaddr);
  EXPECT_EQ(fanout.publisher_stream_id, after.publisher_stream_id);
  EXPECT_TRUE(fanout.quote_id.empty());

  auto encoded = CallControlCodec::EncodeSfuAttach(fanout);
  ASSERT_TRUE(encoded);
  auto decoded = CallControlCodec::DecodeSfuAttach(*encoded);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(decoded->quote_id.empty());
  EXPECT_EQ(decoded->hop_peer_id, "12D3KooWHop");
}

TEST(SfuAttachFanoutTest, PublisherStreamIdStable) {
  EXPECT_EQ(PublisherStreamIdForIdentity("relay:alice"),
            PublisherStreamIdForIdentity("relay:alice"));
  EXPECT_NE(PublisherStreamIdForIdentity("relay:alice"),
            PublisherStreamIdForIdentity("relay:bob"));
  EXPECT_EQ(PublisherStreamIdForIdentity(""), 1u);
}

} // namespace
} // namespace pbr
