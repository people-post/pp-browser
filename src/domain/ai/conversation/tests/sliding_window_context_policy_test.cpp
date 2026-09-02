#include "domain/ai/conversation/Conversation.h"
#include "domain/ai/conversation/SlidingWindowContextPolicy.h"
#include "domain/ai/conversation/TurnCoordinator.h"
#include "domain/ai/conversation/UserMessageFormatter.h"

#include <gtest/gtest.h>

#include <string>

namespace {

pbr::TranscriptEntry& AddCompletedTurn(pbr::Conversation& conversation, const std::string& user_text,
                                       const std::string& assistant_raw) {
  pbr::TranscriptEntry& entry = conversation.AppendUser(user_text);
  conversation.CompleteTurn(entry.id, assistant_raw);
  return entry;
}

void ExpectUserAssistantPair(const pbr::ContextBuildResult& built, const std::string& user_text,
                             const std::string& assistant_text) {
  bool saw_user = false;
  bool saw_assistant = false;
  for (const pbr::ChatMessage& message : built.messages) {
    if (message.role == "user" && message.content == user_text) {
      saw_user = true;
    }
    if (message.role == "assistant" && message.content == assistant_text) {
      saw_assistant = true;
    }
  }
  EXPECT_TRUE(saw_user);
  EXPECT_TRUE(saw_assistant);
}

} // namespace

TEST(SlidingWindowContextPolicyTest, ConversationContextScenarios) {
  pbr::Conversation conversation;
  pbr::SlidingWindowContextPolicy policy;
  const pbr::ContextBudget budget{.max_turn_pairs = 2, .max_recent_chars = 1000, .max_input_tokens = 8000};

  AddCompletedTurn(conversation, "first user", "first assistant");
  AddCompletedTurn(conversation, "second user", "second assistant");
  AddCompletedTurn(conversation, "third user", "third assistant");

  pbr::TranscriptEntry& current = conversation.AppendUser("fourth user");
  const pbr::ContextBuildResult built =
      policy.Build("system prompt", conversation, current, budget);

  ASSERT_FALSE(built.messages.empty());
  EXPECT_EQ(built.messages.front().role, "system");
  EXPECT_EQ(built.messages.front().content, "system prompt");
  EXPECT_EQ(built.messages.back().role, "user");
  EXPECT_EQ(built.messages.back().content, "fourth user");

  ExpectUserAssistantPair(built, "second user", "second assistant");
  ExpectUserAssistantPair(built, "third user", "third assistant");

  bool saw_first = false;
  for (const pbr::ChatMessage& message : built.messages) {
    if (message.role == "user" && message.content == "first user") {
      saw_first = true;
    }
  }
  EXPECT_FALSE(saw_first);

  pbr::Conversation trimmed_conversation;
  for (int i = 0; i < 5; ++i) {
    AddCompletedTurn(trimmed_conversation, "user-" + std::to_string(i), std::string(500, 'a') + std::to_string(i));
  }
  pbr::TranscriptEntry& trimmed_current = trimmed_conversation.AppendUser("current");
  const pbr::ContextBudget tight_budget{.max_turn_pairs = 10, .max_recent_chars = 600, .max_input_tokens = 8000};
  const pbr::ContextBuildResult trimmed =
      policy.Build("system", trimmed_conversation, trimmed_current, tight_budget);
  EXPECT_GT(trimmed.provenance.trimmed_turn_count, 0);

  pbr::Conversation summary_conversation;
  summary_conversation.SetSummary({.text = "User prefers blue.", .version = 1});
  pbr::TranscriptEntry& summary_current = summary_conversation.AppendUser("follow up");
  const pbr::ContextBuildResult with_summary =
      policy.Build("system", summary_conversation, summary_current, budget);
  EXPECT_TRUE(with_summary.provenance.summary_included);
  bool saw_summary = false;
  int system_count = 0;
  for (const pbr::ChatMessage& message : with_summary.messages) {
    if (message.role == "system") {
      ++system_count;
      if (message.content.find("Conversation summary:") != std::string::npos &&
          message.content.find("system") != std::string::npos) {
        saw_summary = true;
      }
    }
  }
  EXPECT_EQ(system_count, 1);
  EXPECT_TRUE(saw_summary);

  pbr::TurnCoordinator coordinator;
  pbr::Conversation turn_conversation;
  AddCompletedTurn(turn_conversation, "hello", "hi there");
  pbr::TranscriptEntry& turn_current = turn_conversation.AppendUser("again");
  const pbr::TurnSnapshot snapshot = coordinator.BeginTurn(turn_conversation, "system", turn_current, budget);
  EXPECT_EQ(snapshot.entry_id, turn_current.id);
  EXPECT_FALSE(snapshot.messages.empty());
  EXPECT_TRUE(coordinator.CompleteTurn(turn_conversation, turn_current.id, "second reply"));
  EXPECT_EQ(turn_conversation.CompletedTurnCount(), 2);

  conversation.StartNewConversation();
  EXPECT_TRUE(conversation.Entries().empty());
  EXPECT_EQ(conversation.CompletedTurnCount(), 0);

  pbr::Conversation payload_conversation;
  pbr::TranscriptEntry& payload_entry = payload_conversation.AppendUser(
      "Book for Alice on 2026-06-15",
      R"({"type":"form_submission","form_id":"booking","values":{"name":"Alice","date":"2026-06-15"}})");
  payload_conversation.CompleteTurn(payload_entry.id, "Confirmed.");
  const pbr::ContextBuildResult payload_history =
      policy.Build("system", payload_conversation, payload_entry, budget);
  bool saw_payload_fence = false;
  for (const pbr::ChatMessage& message : payload_history.messages) {
    if (message.role == "user" && message.content.find("```json") != std::string::npos &&
        message.content.find("form_submission") != std::string::npos) {
      saw_payload_fence = true;
    }
  }
  EXPECT_TRUE(saw_payload_fence);

  pbr::Conversation current_payload_conversation;
  pbr::TranscriptEntry& current_payload = current_payload_conversation.AppendUser(
      "Submit booking",
      R"({"type":"form_submission","form_id":"booking","values":{"name":"Bob"}})");
  const pbr::ContextBuildResult current_payload_built =
      policy.Build("system", current_payload_conversation, current_payload, budget);
  EXPECT_EQ(current_payload_built.messages.back().role, "user");
  EXPECT_NE(current_payload_built.messages.back().content.find("```json"), std::string::npos);
  EXPECT_NE(current_payload_built.messages.back().content.find("Submit booking"), std::string::npos);

  pbr::TranscriptEntry plain_entry;
  plain_entry.user_text = "hello";
  EXPECT_EQ(pbr::FormatUserContentForLlm(plain_entry), "hello");
}
