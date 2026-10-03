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

TEST(MessageRouterTest, OnlyTypelessToolPayloadsInAiThreadsGoToTheAgent) {
  using pbr::MessageRouter;
  using pbr::ThreadKind;
  const std::optional<std::string> tool(R"({"tool":"blog_articles","size":10})");
  EXPECT_TRUE(MessageRouter::IsAgentToolPayload(ThreadKind::Ai, tool));
  // A model-written button in a peer thread must not be treated as agent work.
  EXPECT_FALSE(MessageRouter::IsAgentToolPayload(ThreadKind::Direct, tool));
  EXPECT_FALSE(MessageRouter::IsAgentToolPayload(ThreadKind::Group, tool));
  EXPECT_FALSE(MessageRouter::IsAgentToolPayload(ThreadKind::Ai, std::string(R"({"type":"add_contact","tool":"x"})")));
  EXPECT_FALSE(MessageRouter::IsAgentToolPayload(ThreadKind::Ai, std::string(R"({"type":"show_contact"})")));
  EXPECT_FALSE(MessageRouter::IsAgentToolPayload(ThreadKind::Ai, std::string(R"({"tool":""})")));
  EXPECT_FALSE(MessageRouter::IsAgentToolPayload(ThreadKind::Ai, std::string("not json")));
  EXPECT_FALSE(MessageRouter::IsAgentToolPayload(ThreadKind::Ai, std::nullopt));
}
