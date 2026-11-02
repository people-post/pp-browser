#pragma once

#include "foundation/data/LlmConfig.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/PbrCompat.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct ChatMessage {
  std::string role;
  std::string content;
  std::optional<std::string> tool_call_id;
  std::optional<Value> tool_calls;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  Object parameters;
};

struct ChatCompletionRequest {
  std::vector<ChatMessage> messages;
  std::vector<ToolDefinition> tools;
};

struct ToolCall {
  std::string id;
  std::string name;
  Object arguments;
};

struct ChatCompletionResponse {
  std::optional<std::string> content;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason;
};

/** Which limit a 429 from the AI gateway hit; picks the words the user reads. */
enum class LlmLimitKind { GuestDaily, PerMinute, Other };

/**
 * Machine-readable codes of a busy server (brief_AI contract section 8), in a 429 body or an SSE `error` event.
 * Only kServerBusyCode is worth retrying (BriefTurnRunner, once); the other two are the gateway's per-user limits.
 */
inline constexpr const char* kServerBusyCode = "server_busy";
inline constexpr const char* kUserConcurrencyCode = "user_concurrency";
inline constexpr const char* kRateLimitedCode = "rate_limited";

class LlmClient : public Module {
public:
  explicit LlmClient(LlmConfig config);
  LlmClient(const LlmClient& other);

  Roe<std::string> Complete(const std::string& system_prompt, const std::string& user_prompt) const;
  Roe<ChatCompletionResponse> Complete(const ChatCompletionRequest& request) const;

  // Maps a non-2xx HTTP status and its body to an app error (shared with SseClient).
  static Error MapHttpError(long http_code, const std::string& response_body);

  // Which limit a 429 hit, from the gateway's message (its code is the same for every limit).
  static LlmLimitKind ClassifyLimit(const std::string& api_message);
  /**
   * The machine code of a 429 body: its `code` (top level, or the error object's when it is one of the three), and
   * kRateLimitedCode when it carries none (the gateway's 429s carry none today).
   */
  static std::string LimitCodeOf429(const std::string& response_body);
  // A limit message from the Brief gateway (English; never shown as is).
  static bool IsBriefGatewayLimit(const std::string& api_message);

  static Roe<ChatCompletionResponse> ParseChatCompletionResponse(const std::string& response);

  // Merge leading consecutive system messages into one. Safe for strict chat templates (e.g. Qwen).
  static std::vector<ChatMessage> CoalesceLeadingSystemMessages(std::vector<ChatMessage> messages);

  /**
   * Tests: called with every completions request body just before it is sent (any thread), so a test can count
   * model calls and read what they would carry. Empty to remove.
   */
  static void SetRequestObserverForTests(std::function<void(const std::string& body)> observer);

private:
  LlmConfig config_;
};

} // namespace pbr
