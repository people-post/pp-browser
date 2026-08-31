#include "feature/ai/TurnPlanner.h"

#include "base/ai/PromptBuilder.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::vector<ChatMessage> BuildPlannerMessages(const std::vector<ChatMessage>& context_messages,
                                              const std::string& planner_prompt, const bool repair,
                                              const std::string& invalid_output, const std::string& error_message) {
  std::vector<ChatMessage> messages;
  messages.reserve(context_messages.size() + (repair ? 2 : 0) + 1);
  messages.push_back(ChatMessage{.role = "system", .content = planner_prompt});

  for (const ChatMessage& message : context_messages) {
    if (message.role == "system") {
      continue;
    }
    messages.push_back(message);
  }

  if (repair) {
    messages.push_back(ChatMessage{.role = "assistant", .content = invalid_output});
    messages.push_back(ChatMessage{.role = "user", .content = PromptBuilder::BuildPlannerRepairPrompt(error_message)});
  }

  return messages;
}

} // namespace

Roe<TurnPlan> TurnPlanner::PlanOnce(const LlmClient& llm, const std::vector<ChatMessage>& messages,
                                    const std::string& tools_summary, const std::vector<std::string>& allowed_tools,
                                    const std::string& user_request, const bool repair,
                                    const std::string& invalid_output, const std::string& error_message) {
  const std::string planner_prompt = PromptBuilder::BuildPlannerPrompt(tools_summary);
  std::vector<ChatMessage> request_messages =
      BuildPlannerMessages(messages, planner_prompt, repair, invalid_output, error_message);

  ChatCompletionRequest request;
  request.messages = std::move(request_messages);

  auto response = llm.Complete(request);
  if (!response) {
    return response.error();
  }
  if (!response->content || response->content->empty()) {
    return Error("Turn planner returned empty content");
  }

  auto plan = ParseTurnPlanFromLlmOutput(*response->content, TurnPlanSource::Planner);
  if (!plan) {
    return plan.error();
  }

  plan->user_request = user_request;
  return ValidateTurnPlan(std::move(*plan), allowed_tools);
}

Roe<TurnPlan> TurnPlanner::Plan(const LlmClient& llm, const std::vector<ChatMessage>& context_messages,
                                const std::string& tools_summary, const std::vector<std::string>& allowed_tools,
                                const std::string& user_request) {
  const std::string planner_prompt = PromptBuilder::BuildPlannerPrompt(tools_summary);

  ChatCompletionRequest first_request;
  first_request.messages = BuildPlannerMessages(context_messages, planner_prompt, false, {}, {});

  auto first_response = llm.Complete(first_request);
  if (!first_response) {
    return first_response.error();
  }
  if (!first_response->content || first_response->content->empty()) {
    return Error("Turn planner returned empty content");
  }

  const std::string first_output = *first_response->content;
  auto plan = ParseTurnPlanFromLlmOutput(first_output, TurnPlanSource::Planner);
  if (!plan) {
    const std::string first_error = plan.error().message;
    auto repaired =
        PlanOnce(llm, context_messages, tools_summary, allowed_tools, user_request, true, first_output, first_error);
    if (repaired) {
      return repaired;
    }
    return Error("Turn planner failed: " + first_error + "; repair failed: " + repaired.error().message);
  }

  plan->user_request = user_request;
  auto validated = ValidateTurnPlan(std::move(*plan), allowed_tools);
  if (!validated) {
    const std::string validation_error = validated.error().message;
    auto repaired = PlanOnce(llm, context_messages, tools_summary, allowed_tools, user_request, true, first_output,
                             validation_error);
    if (repaired) {
      return repaired;
    }
    return Error("Turn planner validation failed: " + validation_error);
  }

  return validated;
}

} // namespace pbr
