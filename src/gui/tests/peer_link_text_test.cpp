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
  link.status_label = "Retrying soon (3s)";
  link.banner_message = "amp link manager: dial timeout [link: amp link: dial timeout]";
  link.show_banner = true;
  link.show_retry = true;
  return link;
}

} // namespace

TEST(PeerLinkTextTest, LiveLinksShowAStatusAndNoBanner) {
  for (const auto kind : {ThreadPeerPathKind::Direct, ThreadPeerPathKind::ViaHop, ThreadPeerPathKind::ViaRelay,
                          ThreadPeerPathKind::Connecting, ThreadPeerPathKind::Ready}) {
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
