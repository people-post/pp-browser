// Transport failure text never reaches the user as is: it maps to a locale key, or passes through
// only when it is already a feature's sentence (B89, 2026-10-23).
#include "domain/calls/CallMediaErrorText.h"

#include <gtest/gtest.h>
#include <string>

namespace pbr {
namespace {

TEST(CallMediaErrorTextTest, ReconnectWindowExpiry) {
  EXPECT_STREQ(CallMediaErrorUserKey("amp call-media: no path within the reconnect window"),
               "call.error.reconnect_failed");
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
  EXPECT_STREQ(CallMediaErrorUserKey("something odd [detail]"), "call.error.media_failed");
}

TEST(CallMediaErrorTextTest, UserCopyAndPaymentPassThrough) {
  EXPECT_EQ(CallMediaErrorUserKey("无法完成连接 — 通话密钥未到达。请重试，或确认双方设备在线。"), nullptr);
  EXPECT_EQ(CallMediaErrorUserKey("payment_unavailable:media_relay"), nullptr);
  EXPECT_EQ(CallMediaErrorUserKey(""), nullptr);
}

} // namespace
} // namespace pbr
