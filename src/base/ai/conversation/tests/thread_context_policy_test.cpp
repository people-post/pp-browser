#include "base/ai/conversation/ThreadContextPolicy.h"
#include "base/messaging/ThreadMemoryTypes.h"
#include "base/messaging/ThreadTypes.h"

#include <gtest/gtest.h>

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

} // namespace
} // namespace pbr
