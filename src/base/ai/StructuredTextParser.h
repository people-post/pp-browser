#pragma once

#include "base/ai/TurnPlan.h"
#include "base/ui/WorkingSetTypes.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct ParsedChatAction {
  std::string label;
  std::string message;
  std::optional<std::string> payload;
};

enum class WidgetInitKind { Form, Calendar };

struct WidgetInit {
  WidgetInitKind kind = WidgetInitKind::Form;
  Object config;
};

struct ParseResult {
  bool ok = false;
  std::string rml;
  std::vector<ParsedChatAction> chat_actions;
  std::vector<WidgetInit> widget_inits;
  std::vector<std::string> warnings;
  std::vector<WorkingSetCandidate> working_set_candidates;
  std::string error;
};

struct EmbeddedToolCall {
  std::string name;
  Object arguments;
};

class StructuredTextParser {
public:
  static std::string EscapeText(const std::string& text);
  static std::string EscapeExpressionString(const std::string& text);
  static ParseResult ParseBlocksJson(const std::string& json, ResponseGoal goal = ResponseGoal::General,
                                     RenderMode render_mode = RenderMode::Blocks);
  static ParseResult ParseFromLlmOutput(const std::string& llm_output, ResponseGoal goal = ResponseGoal::General,
                                        RenderMode render_mode = RenderMode::Blocks);
  static bool IsBlocksJsonDocument(const std::string& text);

  static std::optional<std::vector<EmbeddedToolCall>> ExtractEmbeddedToolCalls(const std::string& llm_output);
};

} // namespace pbr
