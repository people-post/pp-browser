#include "base/ai/conversation/SlidingWindowContextPolicy.h"

#include "base/ai/conversation/UserMessageFormatter.h"

#include <algorithm>
#include <cmath>

namespace pbr {

namespace {

int EstimateTokens(const std::string& text) {
  return static_cast<int>(std::ceil(text.size() / 4.0));
}

int EstimateMessagesTokens(const std::vector<ChatMessage>& messages) {
  int total = 0;
  for (const ChatMessage& message : messages) {
    total += EstimateTokens(message.content) + 4;
  }
  return total;
}

std::string TrimTextToCharBudget(const std::string& text, int max_chars) {
  if (static_cast<int>(text.size()) <= max_chars) {
    return text;
  }
  if (max_chars <= 3) {
    return text.substr(0, static_cast<size_t>(max_chars));
  }
  return text.substr(0, static_cast<size_t>(max_chars - 3)) + "...";
}

struct HistoryMessage {
  std::string entry_id;
  std::string role;
  std::string content;
};

std::vector<HistoryMessage> TakeRecentTurnPairs(const std::vector<TranscriptEntry>& completed,
                                                int recent_turn_pairs) {
  if (completed.empty() || recent_turn_pairs <= 0) {
    return {};
  }

  std::vector<HistoryMessage> flattened;
  flattened.reserve(completed.size() * 2);
  for (const TranscriptEntry& entry : completed) {
    flattened.push_back(
        HistoryMessage{.entry_id = entry.id, .role = "user", .content = FormatUserContentForLlm(entry)});
    if (entry.assistant_raw) {
      flattened.push_back(
          HistoryMessage{.entry_id = entry.id, .role = "assistant", .content = *entry.assistant_raw});
    }
  }

  std::vector<HistoryMessage> result;
  int pairs = 0;
  for (auto it = flattened.rbegin(); it != flattened.rend(); ++it) {
    result.insert(result.begin(), *it);
    if (it->role == "user") {
      ++pairs;
      if (pairs >= recent_turn_pairs) {
        break;
      }
    }
  }

  if (!result.empty() && result.front().role == "assistant") {
    result.erase(result.begin());
  }

  return result;
}

std::vector<HistoryMessage> TrimRecentFromStart(std::vector<HistoryMessage> recent, int max_chars, bool& trimmed) {
  int total_chars = 0;
  for (const HistoryMessage& message : recent) {
    total_chars += static_cast<int>(message.content.size());
  }
  if (total_chars <= max_chars) {
    trimmed = false;
    return recent;
  }

  trimmed = true;
  while (!recent.empty() && total_chars > max_chars) {
    total_chars -= static_cast<int>(recent.front().content.size());
    recent.erase(recent.begin());
  }
  return recent;
}

} // namespace

ContextBuildResult SlidingWindowContextPolicy::Build(const std::string& system_prompt,
                                                      const Conversation& conversation,
                                                      const TranscriptEntry& current_turn,
                                                      const ContextBudget& budget) const {
  ContextBuildResult result;
  ContextProvenance provenance;

  std::vector<ChatMessage> prefix;
  std::string system_content = system_prompt;
  if (const auto& summary = conversation.Summary(); summary && !summary->text.empty()) {
    const std::string trimmed = TrimTextToCharBudget(summary->text, budget.max_summary_chars);
    if (!system_content.empty()) {
      system_content += "\n\n";
    }
    system_content += "Conversation summary:\n" + trimmed;
    provenance.summary_included = true;
  }
  prefix.push_back(ChatMessage{.role = "system", .content = std::move(system_content)});

  const std::vector<TranscriptEntry> completed = conversation.CompletedEntries();
  bool char_trimmed = false;
  std::vector<HistoryMessage> recent =
      TrimRecentFromStart(TakeRecentTurnPairs(completed, budget.max_turn_pairs), budget.max_recent_chars, char_trimmed);
  if (char_trimmed) {
    ++provenance.trimmed_turn_count;
  }

  for (const HistoryMessage& message : recent) {
    result.messages.push_back(ChatMessage{.role = message.role, .content = message.content});
    if (std::find(provenance.included_entry_ids.begin(), provenance.included_entry_ids.end(), message.entry_id) ==
        provenance.included_entry_ids.end()) {
      provenance.included_entry_ids.push_back(message.entry_id);
    }
  }

  result.messages.insert(result.messages.begin(), prefix.begin(), prefix.end());
  result.messages.push_back(
      ChatMessage{.role = "user", .content = FormatUserContentForLlm(current_turn)});

  const int max_tokens = static_cast<int>(std::floor(budget.max_input_tokens * budget.token_estimate_margin));
  while (EstimateMessagesTokens(result.messages) > max_tokens && recent.size() > 2) {
    recent.erase(recent.begin());
    ++provenance.trimmed_turn_count;

    result.messages.clear();
    result.messages.insert(result.messages.begin(), prefix.begin(), prefix.end());
    for (const HistoryMessage& message : recent) {
      result.messages.push_back(ChatMessage{.role = message.role, .content = message.content});
    }
    result.messages.push_back(
        ChatMessage{.role = "user", .content = FormatUserContentForLlm(current_turn)});
  }

  provenance.estimated_input_tokens =
      static_cast<int>(std::ceil(EstimateMessagesTokens(result.messages) * budget.token_estimate_margin));
  result.provenance = provenance;
  return result;
}

} // namespace pbr
