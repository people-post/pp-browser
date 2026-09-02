#include "domain/net/BriefGuestLlmClient.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(BriefGuestLlmClientTest, ResolvePrefersRegisteredOverGuest) {
  EXPECT_EQ(ResolveBriefLlmApiKey("brf_llm_a", "brf_guest_b"), "brf_llm_a");
  EXPECT_EQ(ResolveBriefLlmApiKey("", "brf_guest_b"), "brf_guest_b");
  EXPECT_EQ(ResolveBriefLlmApiKey("", ""), "");
}

TEST(BriefGuestLlmClientTest, MintRequiresBaseUrl) {
  auto missing = MintBriefGuestLlmKey("");
  ASSERT_FALSE(static_cast<bool>(missing));
}

} // namespace
} // namespace pbr
