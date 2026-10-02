#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "common/PbrCompat.h"
#include "foundation/data/LlmConfig.h"

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

struct BriefAiHistoryTurn {
  std::string role; // "user" | "assistant"
  std::string content;
};

struct BriefAiRequest {
  std::string message;
  std::optional<std::string> intent; // omitted from the JSON when nullopt
  std::vector<BriefAiHistoryTurn> history; // oldest first; at most the last 6 are sent
  std::string summary; // omitted when empty
  std::string app_version, platform, lang; // client.version / platform / lang (omitted when empty)
  std::vector<std::string> capabilities; // client.capabilities (omitted when empty)
};

struct BriefAiSource {
  std::string title, url, kind;
};

struct BriefAiEvent {
  enum class Type { Meta, Status, Token, Done, Handoff, Error };
  Type type = Type::Meta;
  std::string route; // Meta, Done
  std::string phase, tool, query; // Status
  std::string delta; // Token
  std::string response, finish; // Done
  std::vector<BriefAiSource> sources; // Done
  std::string capability; // Handoff (empty when the server sent null)
  std::string code, message; // Error
  bool retryable = false; // Error
};

enum class BriefAiOutcome { Done, Handoff, Error, Cancelled };

// Streams one answer from brief_AI (contract: brief_AI/docs/contracts/pp-client.md).
class BriefAiClient : public Module {
public:
  // Stream endpoint relative to LlmConfig::base_url. The gateway path is not final (contract §十二 P3).
  static constexpr std::string_view kStreamPath = "/pp/chat/stream";

  // `stream_url` overrides base_url + kStreamPath (dev: a local fake server).
  explicit BriefAiClient(LlmConfig config, std::string stream_url = {});

  // Blocks until the stream ends; `on_event` runs on the calling thread, in stream order.
  // A stream that ends without done/handoff/error is an "answer interrupted" HttpError.
  Roe<BriefAiOutcome> Stream(const BriefAiRequest& request, const std::function<void(const BriefAiEvent&)>& on_event,
                             const std::atomic<bool>& cancel) const;

  static std::string BuildRequestJson(const BriefAiRequest& request);
  // nullopt: "[DONE]", an unknown type, or not JSON.
  static std::optional<BriefAiEvent> ParseEvent(const std::string& data);

private:
  LlmConfig config_;
  std::string stream_url_;
};

} // namespace pbr
