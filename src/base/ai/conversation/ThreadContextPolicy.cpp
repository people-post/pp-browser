#include "base/ai/conversation/ThreadContextPolicy.h"

#include "base/ai/conversation/UserMessageFormatter.h"

#include <sstream>

namespace pbr {

namespace {

std::string FormatThreadLine(const ThreadMessage& message) {
  std::ostringstream out;
  out << message.sender_contact_id << ": " << message.text;
  if (message.content_rml) {
    out << " [rich]";
  }
  return out.str();
}

int EstimateTokens(const std::string& text) {
  return static_cast<int>(text.size() / 4) + 1;
}

} // namespace

ThreadContextPolicy::ThreadContextPolicy(ContextBudget budget) : budget_(budget) {}

ContextBuildResult ThreadContextPolicy::Build(const std::vector<ThreadMessage>& messages,
                                              const std::string& system_prompt, const std::string& current_user_text,
                                              const std::optional<std::string>& current_user_payload) const {
  ContextBuildResult result;
  result.messages.push_back(ChatMessage{.role = "system", .content = system_prompt});

  int char_budget = budget_.max_recent_chars;
  std::vector<std::string> lines;
  for (auto it = messages.rbegin(); it != messages.rend() && static_cast<int>(lines.size()) < budget_.max_turn_pairs * 2;
       ++it) {
    const std::string line = FormatThreadLine(*it);
    if (char_budget - static_cast<int>(line.size()) < 0) {
      break;
    }
    char_budget -= static_cast<int>(line.size());
    lines.push_back(line);
    result.provenance.included_entry_ids.push_back(it->id);
  }

  std::ostringstream transcript;
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    transcript << *it << "\n";
  }
  if (!transcript.str().empty()) {
    result.messages.push_back(
        ChatMessage{.role = "user", .content = "Thread transcript:\n" + transcript.str()});
    result.messages.push_back(
        ChatMessage{.role = "assistant", .content = "Understood. I have the thread context."});
  }

  std::string user_content = current_user_text;
  if (current_user_payload && !current_user_payload->empty()) {
    user_content +=
        "\n\nStructured action context (supports the user message above; user_text is primary):\n```json\n" +
        *current_user_payload + "\n```";
  }
  result.messages.push_back(ChatMessage{.role = "user", .content = user_content});

  for (const ChatMessage& message : result.messages) {
    result.provenance.estimated_input_tokens += EstimateTokens(message.content);
  }
  return result;
}

std::vector<ChatMessage> ThreadContextPolicy::BuildAssistContext(const std::vector<ThreadMessage>& messages,
                                                                 const std::string& prompt) const {
  std::ostringstream transcript;
  int char_budget = budget_.max_recent_chars;
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    const std::string line = FormatThreadLine(*it);
    if (char_budget - static_cast<int>(line.size()) < 0) {
      break;
    }
    char_budget -= static_cast<int>(line.size());
    transcript << line << "\n";
  }

  std::vector<ChatMessage> out;
  out.push_back(ChatMessage{.role = "system",
                            .content = "You are assisting in a person-to-person chat. Use the transcript for "
                                       "context. Reply concisely."});
  if (!transcript.str().empty()) {
    out.push_back(ChatMessage{.role = "user", .content = "Transcript:\n" + transcript.str()});
  }
  out.push_back(ChatMessage{.role = "user", .content = prompt});
  return out;
}

} // namespace pbr
