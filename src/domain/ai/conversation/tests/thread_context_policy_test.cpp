#include "domain/ai/conversation/ThreadContextPolicy.h"
#include "common/thread/ThreadMemoryTypes.h"
#include "common/thread/ThreadTypes.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(ThreadContextPolicyTest, InjectsThreadMemorySummary) {
  ThreadContextPolicy policy;

  ThreadMessage prior;
  prior.id = "m1";
  prior.sender_contact_id = kLocalSelfContactId;
  prior.text = "recent question";

  ConversationSummary summary;
  summary.text = "User prefers dark mode.";
  summary.version = 1;

  const ContextBuildResult built = policy.Build({prior}, "system prompt", "follow up", std::nullopt, summary);
  ASSERT_TRUE(built.provenance.summary_included);

  int system_count = 0;
  bool saw_summary = false;
  for (const ChatMessage& message : built.messages) {
    if (message.role == "system") {
      ++system_count;
      if (message.content.find("Conversation summary:") != std::string::npos &&
          message.content.find("dark mode") != std::string::npos &&
          message.content.find("system prompt") != std::string::npos) {
        saw_summary = true;
      }
    }
  }
  EXPECT_EQ(system_count, 1);
  EXPECT_TRUE(saw_summary);
  EXPECT_EQ(built.messages.back().content, "follow up");
}

// In-chat @ai is a user-facing privacy promise ("only your question is sent"): pin it.
TEST(ThreadContextPolicyTest, AssistContextIsSystemPlusTheQuestionOnly) {
  ThreadContextPolicy policy;
  const std::vector<ChatMessage> messages = policy.BuildAssistContext("what is the gold price");
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].role, "system");
  EXPECT_EQ(messages[1].role, "user");
  EXPECT_EQ(messages[1].content, "what is the gold price");
}

TEST(ThreadContextPolicyTest, TranscriptUsesRoleLabelsNotContactIds) {
  ThreadContextPolicy policy;
  ThreadMessage mine;
  mine.id = "m1";
  mine.sender_contact_id = kLocalSelfContactId;
  mine.text = "my line";
  ThreadMessage theirs;
  theirs.id = "m2";
  theirs.sender_contact_id = "contact:QmPeerIdThatMustNotLeak";
  theirs.text = "their line";
  ThreadMessage ai;
  ai.id = "m3";
  ai.sender_contact_id = kAiAssistantContactId;
  ai.text = "assistant line";

  const ContextBuildResult built = policy.Build({mine, theirs, ai}, "system prompt", "follow up");
  std::string all;
  for (const ChatMessage& message : built.messages) {
    all += message.content + "\n";
  }
  EXPECT_NE(all.find("user: my line"), std::string::npos) << all;
  EXPECT_NE(all.find("peer: their line"), std::string::npos) << all;
  EXPECT_NE(all.find("assistant: assistant line"), std::string::npos) << all;
  EXPECT_EQ(all.find("QmPeerIdThatMustNotLeak"), std::string::npos) << all;
  EXPECT_EQ(all.find(kLocalSelfContactId), std::string::npos) << all;
}

} // namespace
} // namespace pbr
