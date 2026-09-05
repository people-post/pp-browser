#include "foundation/platform/PlatformUserHints.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

TEST(PlatformUserHintsTest, P2pNetworkHintKeyNonEmpty) {
  const std::string_view key = PlatformUserHints::P2pNetworkHintKey();
  EXPECT_FALSE(key.empty());
  EXPECT_NE(std::string(key).find("hints.network."), std::string::npos);
}

TEST(PlatformUserHintsTest, MicBlockedHintKey) {
  EXPECT_EQ(PlatformUserHints::MicBlockedHintKey(), "hints.mic_blocked");
}

} // namespace
} // namespace pbr
