#include "gui/chat/PeerLinkText.h"

#include <gtest/gtest.h>

#include <string>

using pbr::PeerLinkTextFor;
using pbr::ThreadPeerLinkView;
using pbr::ThreadPeerPathKind;

namespace {

ThreadPeerLinkView Link(const ThreadPeerPathKind kind, const bool relay_available = false) {
  ThreadPeerLinkView link;
  link.path_kind = kind;
  link.relay_available = relay_available;
  // What the orchestrator fills today; none of it may reach the user.
  link.detail = "amp link manager: dial timeout [link: amp link: dial timeout]";
  link.show_retry = true;
  return link;
}

} // namespace

TEST(PeerLinkTextTest, LiveLinksShowAStatusAndNoBanner) {
  for (const auto kind : {ThreadPeerPathKind::Direct, ThreadPeerPathKind::ViaHop, ThreadPeerPathKind::ViaRelay,
                          ThreadPeerPathKind::Connecting}) {
    const auto text = PeerLinkTextFor(Link(kind));
    EXPECT_NE(text.status_key, nullptr);
    EXPECT_EQ(text.banner_key, nullptr);
    EXPECT_FALSE(text.show_retry);
  }
}

TEST(PeerLinkTextTest, FailedDirectLinkIsSilentWhileRelayCarriesMessages) {
  const auto text = PeerLinkTextFor(Link(ThreadPeerPathKind::Degraded, true));
  EXPECT_STREQ(text.status_key, "chat.link.via_relay");
  EXPECT_EQ(text.banner_key, nullptr);
  EXPECT_FALSE(text.show_retry);
  // The badge must agree with the text: relay icon, not the amber "degraded" one.
  EXPECT_EQ(text.display_kind, ThreadPeerPathKind::ViaRelay);
  EXPECT_EQ(PeerLinkTextFor(Link(ThreadPeerPathKind::Degraded, false)).display_kind, ThreadPeerPathKind::Degraded);
  EXPECT_EQ(PeerLinkTextFor(Link(ThreadPeerPathKind::Direct)).display_kind, ThreadPeerPathKind::Direct);
}

TEST(PeerLinkTextTest, UnreachablePeerWithoutRelayOffersRetry) {
  const auto text = PeerLinkTextFor(Link(ThreadPeerPathKind::Degraded, false));
  EXPECT_STREQ(text.status_key, "chat.link.retrying");
  EXPECT_STREQ(text.banner_key, "chat.link.banner.unreachable");
  EXPECT_TRUE(text.show_retry);
}

TEST(PeerLinkTextTest, MissingAddressNamesTheCause) {
  const auto text = PeerLinkTextFor(Link(ThreadPeerPathKind::Failed));
  EXPECT_STREQ(text.status_key, "chat.link.offline");
  EXPECT_STREQ(text.banner_key, "chat.link.banner.no_address");
  EXPECT_FALSE(text.show_retry);
}

TEST(PeerLinkTextTest, NoStateShowsNothing) {
  const auto text = PeerLinkTextFor(Link(ThreadPeerPathKind::None));
  EXPECT_EQ(text.status_key, nullptr);
  EXPECT_EQ(text.banner_key, nullptr);
}

// The header's line per state: direct says 直连, hop and relay say 加密中转, with the badge matching the text.
TEST(PeerLinkTextTest, HeaderTextPerState) {
  EXPECT_STREQ(PeerLinkTextFor(Link(ThreadPeerPathKind::Direct)).status_key, "chat.link.direct");
  EXPECT_STREQ(PeerLinkTextFor(Link(ThreadPeerPathKind::ViaHop)).status_key, "chat.link.via_hop");
  EXPECT_STREQ(PeerLinkTextFor(Link(ThreadPeerPathKind::ViaRelay)).status_key, "chat.link.via_relay");
  EXPECT_STREQ(PeerLinkTextFor(Link(ThreadPeerPathKind::Connecting)).status_key, "chat.link.connecting");
  EXPECT_STREQ(PeerLinkTextFor(Link(ThreadPeerPathKind::Failed)).status_key, "chat.link.offline");
}

// No link is up (idle): with a relay the next message travels by it, so the line says so; with none it says nothing.
TEST(PeerLinkTextTest, IdleLinkSaysRelayWhereARelayCarriesTheNextMessage) {
  const auto with_relay = PeerLinkTextFor(Link(ThreadPeerPathKind::Ready, true));
  EXPECT_STREQ(with_relay.status_key, "chat.link.via_relay");
  EXPECT_EQ(with_relay.display_kind, ThreadPeerPathKind::ViaRelay);
  EXPECT_EQ(with_relay.banner_key, nullptr);
  const auto without_relay = PeerLinkTextFor(Link(ThreadPeerPathKind::Ready, false));
  EXPECT_EQ(without_relay.status_key, nullptr);
  EXPECT_EQ(without_relay.banner_key, nullptr);
}
