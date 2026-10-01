#include "feature/conversations/MessageRouter.h"

#include "common/PlatformLimits.h"
#include "common/thread/ThreadTypes.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

TEST(MessageRouterTest, BoundsStructuredUserPayloadWithoutRestrictingText) {
  const std::optional<std::string> at_limit(std::in_place, pbr::kMaxUserPayloadBytes, 'x');
  const auto accepted = pbr::MessageRouter::ValidateUserPayload(at_limit);
  ASSERT_TRUE(accepted) << accepted.error().message;

  const std::optional<std::string> over_limit(std::in_place, pbr::kMaxUserPayloadBytes + 1, 'x');
  const auto rejected = pbr::MessageRouter::ValidateUserPayload(over_limit);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().message,
            "User payload exceeds limit of " + std::to_string(pbr::kMaxUserPayloadBytes) + " bytes");
}

TEST(MessageRouterTest, AiThreadDropsTheAtAiPrefixAndOtherThreadsKeepTheText) {
  EXPECT_EQ(pbr::MessageRouter::TextForAgent(pbr::ThreadKind::Ai, "@ai+ hello"), "hello");
  EXPECT_EQ(pbr::MessageRouter::TextForAgent(pbr::ThreadKind::Ai, "@ai what time is it"), "what time is it");
  EXPECT_EQ(pbr::MessageRouter::TextForAgent(pbr::ThreadKind::Ai, "plain question"), "plain question");
  EXPECT_EQ(pbr::MessageRouter::TextForAgent(pbr::ThreadKind::Direct, "@ai+ hello"), "@ai+ hello");
}
