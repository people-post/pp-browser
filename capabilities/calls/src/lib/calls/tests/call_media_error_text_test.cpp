// Transport failure text never reaches the user as is: it maps to a locale key, or passes through
// only when it is already a feature's sentence (B89, 2026-10-23).
#include "lib/calls/CallMediaErrorText.h"

#include "common/Localized.h"

#include <gtest/gtest.h>
#include <map>
#include <string>

namespace pbr {
namespace {

TEST(CallMediaErrorTextTest, DroppedAndConnectTimeoutAreToldByTheCallScreen) {
  // #181: the failed page already says "Call dropped" / "Couldn't reach them"; no second prompt.
  EXPECT_TRUE(CallMediaErrorShownOnCallScreen({CallMediaErrorKind::Dropped, "anything"}));
  EXPECT_TRUE(CallMediaErrorShownOnCallScreen({CallMediaErrorKind::ConnectTimeout, "anything"}));
  EXPECT_FALSE(CallMediaErrorShownOnCallScreen({CallMediaErrorKind::Other, "call-media migrate: timed out"}));
  // K014: the kind decides, never the words — an Other naming a drop is still a notice.
  EXPECT_FALSE(CallMediaErrorShownOnCallScreen({CallMediaErrorKind::Other, "amp call-media: call dropped"}));
  EXPECT_FALSE(CallMediaErrorShownOnCallScreen({CallMediaErrorKind::Other, "mesh connect timeout"}));
}

TEST(CallMediaErrorTextTest, PeerNotReachedChains) {
  EXPECT_STREQ(CallMediaErrorUserKey("call-media: peer not reached [reach: peer reach: circuit failed [tunnel: "
                                     "circuit-relay bridge timed out]]"),
               "call.error.peer_unreachable");
}

TEST(CallMediaErrorTextTest, MigrateTimeout) {
  EXPECT_STREQ(CallMediaErrorUserKey("call-media migrate: timed out"), "call.error.migrate_timeout");
}

TEST(CallMediaErrorTextTest, DialFailures) {
  EXPECT_STREQ(CallMediaErrorUserKey("call-media: punch failed [signaling: amp link: too many concurrent dials]"),
               "call.error.dial_failed");
  EXPECT_STREQ(CallMediaErrorUserKey("dial failed [link: amp link: dial aborted]"), "call.error.dial_failed");
}

TEST(CallMediaErrorTextTest, UnknownTransportTextGetsTheGenericSentence) {
  EXPECT_STREQ(CallMediaErrorUserKey("amp circuit-relay: channel open failed"), "call.error.media_failed");
  EXPECT_STREQ(CallMediaErrorUserKey("something odd [relay: refused]"), "call.error.media_failed");
}

TEST(CallMediaErrorTextTest, EnglishUserCopyIsNotMistakenForTransportText) {
  EXPECT_EQ(CallMediaErrorUserKey("Couldn't bring Lee [work] into this call — no shared media path."), nullptr);
  EXPECT_EQ(CallMediaErrorUserKey("The other device is unreachable right now."), nullptr);
}

TEST(CallMediaErrorTextTest, UserCopyAndPaymentPassThrough) {
  EXPECT_EQ(CallMediaErrorUserKey("无法完成连接 — 通话密钥未到达。请重试，或确认双方设备在线。"), nullptr);
  EXPECT_EQ(CallMediaErrorUserKey("payment_unavailable:media_relay"), nullptr);
  EXPECT_EQ(CallMediaErrorUserKey(""), nullptr);
}

TEST(CallMediaErrorTextTest, TheFeaturesOwnLocalizedNoticesPassThroughInBothLanguages) {
  const std::map<std::string, std::string> zh = {{"call.notice.removed", "你已被移出通话"},
                                                 {"call.notice.left_group", "你已不在这个群里"},
                                                 {"call.notice.call_ended", "这通电话已经结束"},
                                                 {"call.error.media_unavailable", "通话音视频暂时不可用。"}};
  const std::map<std::string, std::string> en = {{"call.notice.removed", "You were removed from the call"},
                                                 {"call.notice.left_group", "You are no longer in this group"},
                                                 {"call.notice.call_ended", "That call has ended"},
                                                 {"call.error.media_unavailable", "Call media isn't available."}};
  for (const auto* table : {&zh, &en}) {
    SetTranslator([table](std::string_view key, const TextArgs&) {
      const auto it = table->find(std::string(key));
      return it == table->end() ? std::string(key) : it->second;
    });
    for (const auto& entry : *table) {
      EXPECT_TRUE(CallNoticeIsLocalizedCopy(entry.second)) << entry.second;
    }
    EXPECT_FALSE(CallNoticeIsLocalizedCopy("some other developer text"));
    EXPECT_FALSE(CallNoticeIsLocalizedCopy(""));
  }
  SetTranslator(nullptr);
}

} // namespace
} // namespace pbr
