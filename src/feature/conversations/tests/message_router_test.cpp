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

TEST(MessageRouterTest, AtAiIsUnderstoodInDirectAndGroupThreadsOnly) {
  using pbr::MessageRouter;
  using pbr::ThreadKind;
  EXPECT_TRUE(MessageRouter::HandlesAtAi(ThreadKind::Direct));
  EXPECT_TRUE(MessageRouter::HandlesAtAi(ThreadKind::Group));
  EXPECT_FALSE(MessageRouter::HandlesAtAi(ThreadKind::Ai));
}

// Buttons of the on-device app-action flow: the three whitelisted tools go to the agent (in any thread kind, so a
// list shown after "@ai" in a chat is never relayed to the peer); edit_remark opens the contact page in the UI.
TEST(MessageRouterTest, LocalActionPayloadsGoToTheAgentExceptTheRemarkButton) {
  const std::optional<std::string> open(R"({"type":"local_action","tool":"open_conversation","thread_id":"d1"})");
  const std::optional<std::string> search(R"({"type":"local_action","tool":"search_people","query":"ken"})");
  const std::optional<std::string> remark(R"({"type":"local_action","tool":"edit_remark","contact_id":"c1"})");
  const std::optional<std::string> unknown(R"({"type":"local_action","tool":"set_notifications"})");

  EXPECT_EQ(pbr::MessageRouter::LocalActionTool(open), std::optional<std::string>("open_conversation"));
  EXPECT_EQ(pbr::MessageRouter::LocalActionTool(remark), std::optional<std::string>("edit_remark"));
  EXPECT_TRUE(pbr::MessageRouter::IsAgentLocalActionPayload(open));
  EXPECT_TRUE(pbr::MessageRouter::IsAgentLocalActionPayload(search));
  EXPECT_FALSE(pbr::MessageRouter::IsAgentLocalActionPayload(unknown)); // dropped with a warning, no "thinking"
  EXPECT_FALSE(pbr::MessageRouter::IsAgentLocalActionPayload(remark));

  // Not a local action: other typed payloads, tool payloads without a type, text.
  EXPECT_FALSE(pbr::MessageRouter::LocalActionTool(std::string(R"({"type":"open_conversation","thread_id":"d1"})")));
  EXPECT_FALSE(pbr::MessageRouter::LocalActionTool(std::string(R"({"tool":"search_people"})")));
  EXPECT_FALSE(pbr::MessageRouter::LocalActionTool(std::string("not json")));
  EXPECT_FALSE(pbr::MessageRouter::LocalActionTool(std::nullopt));
  // A typed payload is never an agent tool payload, so it cannot reach the old tool-payload path either.
  EXPECT_FALSE(pbr::MessageRouter::IsAgentToolPayload(pbr::ThreadKind::Ai, open));
}
