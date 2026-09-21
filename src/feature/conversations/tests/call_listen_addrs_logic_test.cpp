#include "domain/messaging/CallListenAddrsLogic.h"

#include "domain/messaging/CallControlCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallListenAddrsLogicTest, FillFromAmpLanMasWithoutMeshPeerIdCallback) {
  // D3: mDNS miss OK when invite embeds Amp LAN MAs (no separate PeerId provider).
  const std::vector<std::string> mas = {
      "/ip4/192.168.1.10/udp/19001/adp/1.0.0/p2p/12D3KooWAliceLan"};
  std::string peer_id;
  std::vector<std::string> out;
  FillCallListenFields(mas, peer_id, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], mas[0]);
  EXPECT_EQ(peer_id, "12D3KooWAliceLan");
  EXPECT_TRUE(InviteListenAddrsSufficientForDirectDial(out));
}

TEST(CallListenAddrsLogicTest, EmptyMasNotSufficientForDirectDial) {
  EXPECT_FALSE(InviteListenAddrsSufficientForDirectDial({}));
  EXPECT_FALSE(InviteListenAddrsSufficientForDirectDial({"/ip4/10.0.0.1/udp/1"})); // no /p2p/
}

TEST(CallListenAddrsLogicTest, DoesNotOverwriteExistingPeerId) {
  std::string peer_id = "12D3KooWExplicit";
  std::vector<std::string> out;
  FillCallListenFields({"/ip4/10.0.0.2/udp/1/p2p/12D3KooWFromMa"}, peer_id, out);
  EXPECT_EQ(peer_id, "12D3KooWExplicit");
}

TEST(CallListenAddrsLogicTest, InviteEncodeRoundTripKeepsListenMas) {
  CallInviteDetail invite;
  invite.call_id = "call:d3";
  invite.media_mode = CallMediaMode::Voice;
  invite.expires_at = 1;
  std::string peer_id;
  FillCallListenFields({"/ip4/10.0.0.3/udp/9/adp/1.0.0/p2p/12D3KooWBob"}, peer_id,
                       invite.listen_multiaddrs);
  invite.libp2p_peer_id = peer_id;
  auto enc = CallControlCodec::EncodeInvite(invite);
  ASSERT_TRUE(enc);
  auto dec = CallControlCodec::DecodeInvite(*enc);
  ASSERT_TRUE(dec);
  ASSERT_EQ(dec->listen_multiaddrs.size(), 1u);
  EXPECT_TRUE(InviteListenAddrsSufficientForDirectDial(dec->listen_multiaddrs));
  EXPECT_EQ(dec->libp2p_peer_id, "12D3KooWBob");
}

TEST(CallListenAddrsLogicTest, Ipv6ListenAddrsSufficientForDirectDial) {
  std::string peer_id;
  std::vector<std::string> out;
  FillCallListenFields({"/ip6/2001:db8::9/udp/19001/adp/1.0.0/p2p/12D3KooWIpv6"}, peer_id, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(peer_id, "12D3KooWIpv6");
  EXPECT_TRUE(InviteListenAddrsSufficientForDirectDial(out));
}

} // namespace
} // namespace pbr
