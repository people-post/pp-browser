#include "agent/StructuredTextParser.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace pbr {

std::string StructuredTextParser::EscapeText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

std::string StructuredTextParser::EscapeExpressionString(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c == '\\' || c == '\'') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

namespace {

std::string TrimAsciiWhitespace(std::string text) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
  text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

/// Close unbalanced `{`/`}` — common when models truncate the outer object.
std::string BalanceJsonBraces(std::string json) {
  int depth = 0;
  for (char c : json) {
    if (c == '{')
      depth++;
    else if (c == '}')
      depth = std::max(0, depth - 1);
  }
  while (depth-- > 0)
    json += '}';
  return json;
}

ParseResult Fail(const std::string& message) {
  ParseResult result;
  result.ok = false;
  result.error = message;
  result.rml = "<p class=\"error\">" + StructuredTextParser::EscapeText(message) + "</p>";
  return result;
}

ParseResult RenderBlock(const nlohmann::json& block) {
  if (!block.is_object() || !block.contains("type") || !block["type"].is_string()) {
    return Fail("Block must be an object with a type field");
  }

  const std::string type = block["type"].get<std::string>();

  if (type == "paragraph") {
    if (!block.contains("text") || !block["text"].is_string()) {
      return Fail("paragraph block requires text");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<p>" + StructuredTextParser::EscapeText(block["text"].get<std::string>()) + "</p>";
    return result;
  }

  if (type == "heading") {
    if (!block.contains("text") || !block["text"].is_string()) {
      return Fail("heading block requires text");
    }
    int level = 2;
    if (block.contains("level")) {
      if (!block["level"].is_number_integer()) {
        return Fail("heading level must be an integer");
      }
      level = block["level"].get<int>();
    }
    if (level < 1 || level > 3) {
      return Fail("heading level must be 1-3");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<h" + std::to_string(level) + ">" + StructuredTextParser::EscapeText(block["text"].get<std::string>()) +
                 "</h" + std::to_string(level) + ">";
    return result;
  }

  if (type == "list") {
    if (!block.contains("items") || !block["items"].is_array()) {
      return Fail("list block requires items array");
    }
    const bool ordered = block.value("ordered", false);
    std::ostringstream out;
    out << (ordered ? "<ol>" : "<ul>");
    for (const auto& item : block["items"]) {
      if (!item.is_string()) {
        return Fail("list items must be strings");
      }
      out << "<li>" << StructuredTextParser::EscapeText(item.get<std::string>()) << "</li>";
    }
    out << (ordered ? "</ol>" : "</ul>");
    ParseResult result;
    result.ok = true;
    result.rml = out.str();
    return result;
  }

  if (type == "code") {
    if (!block.contains("text") || !block["text"].is_string()) {
      return Fail("code block requires text");
    }
    ParseResult result;
    result.ok = true;
    result.rml = "<div class=\"code-block\">" + StructuredTextParser::EscapeText(block["text"].get<std::string>()) + "</div>";
    return result;
  }

  return Fail("Unknown block type: " + type);
}

ParseResult ParseButtonBlock(const nlohmann::json& block) {
  if (!block.contains("label") || !block["label"].is_string()) {
    return Fail("button block requires label");
  }
  if (!block.contains("message") || !block["message"].is_string()) {
    return Fail("button block requires message");
  }
  const std::string message = block["message"].get<std::string>();
  if (message.empty()) {
    return Fail("button message must not be empty");
  }
  if (message.find('\n') != std::string::npos || message.find('\r') != std::string::npos) {
    return Fail("button message must not contain newlines");
  }
  ParseResult result;
  result.ok = true;
  const std::string label = block["label"].get<std::string>();
  result.rml = "<button class=\"chat-suggestion\" data-event-click=\"send_suggestion('" +
               StructuredTextParser::EscapeExpressionString(message) + "')\">" +
               StructuredTextParser::EscapeText(label) + "</button>";
  result.suggestions.push_back({label, message});
  return result;
}

std::optional<std::string> ExtractJsonPayload(const std::string& llm_output) {
  static const std::regex json_re(R"re(```json\s*([\s\S]*?)```)re", std::regex::icase);
  std::smatch match;
  if (std::regex_search(llm_output, match, json_re)) {
    return TrimAsciiWhitespace(match[1].str());
  }

  const std::string trimmed = TrimAsciiWhitespace(llm_output);
  if (!trimmed.empty() && trimmed.front() == '{') {
    return trimmed;
  }
  return std::nullopt;
}

bool IsDisplayBlockType(const std::string& type) {
  return type == "paragraph" || type == "heading" || type == "list" || type == "code" || type == "button";
}

bool IsKnownToolName(const std::string& name) {
  return name == "web_search";
}

bool IsEmbeddedToolBlock(const nlohmann::json& block) {
  if (!block.is_object()) {
    return false;
  }

  if (block.contains("type") && block["type"].is_string()) {
    const std::string type = block["type"].get<std::string>();
    if (IsDisplayBlockType(type)) {
      return false;
    }
    if (type == "tool" || type == "tool_call" || IsKnownToolName(type)) {
      return true;
    }
  }

  if (block.contains("tool") && block["tool"].is_string()) {
    return true;
  }

  if (block.contains("name") && block["name"].is_string() && IsKnownToolName(block["name"].get<std::string>())) {
    return true;
  }

  return false;
}

nlohmann::json ToolArgumentsFromBlock(const nlohmann::json& block) {
  for (const char* key : {"params", "arguments", "parameters"}) {
    if (block.contains(key) && block[key].is_object()) {
      return block[key];
    }
  }
  if (block.contains("query") && block["query"].is_string()) {
    return {{"query", block["query"]}};
  }
  return nlohmann::json::object();
}

std::string ToolNameFromBlock(const nlohmann::json& block) {
  if (block.contains("tool") && block["tool"].is_string()) {
    return block["tool"].get<std::string>();
  }
  if (block.contains("type") && block["type"].is_string()) {
    const std::string type = block["type"].get<std::string>();
    if (IsKnownToolName(type)) {
      return type;
    }
  }
  if (block.contains("name") && block["name"].is_string()) {
    return block["name"].get<std::string>();
  }
  return {};
}

} // namespace

ParseResult StructuredTextParser::ParseBlocksJson(const std::string& json) {
  const std::string trimmed = TrimAsciiWhitespace(json);
  nlohmann::json doc = nlohmann::json::parse(trimmed, nullptr, false);
  if (doc.is_discarded()) {
    const std::string repaired = BalanceJsonBraces(trimmed);
    if (repaired != trimmed)
      doc = nlohmann::json::parse(repaired, nullptr, false);
  }
  if (doc.is_discarded()) {
    return Fail("Invalid JSON");
  }

  if (!doc.is_object() || !doc.contains("blocks") || !doc["blocks"].is_array()) {
    return Fail("JSON must contain a blocks array");
  }

  std::ostringstream text_stack;
  bool has_text = false;

  ParseResult result;
  result.ok = true;

  for (const auto& block : doc["blocks"]) {
    if (block.is_object() && block.contains("type") && block["type"].is_string() &&
        block["type"].get<std::string>() == "button") {
      auto button = ParseButtonBlock(block);
      if (!button.ok) {
        return button;
      }
      result.suggestions.insert(result.suggestions.end(), button.suggestions.begin(), button.suggestions.end());
      text_stack << button.rml;
      has_text = true;
      continue;
    }

    auto rendered = RenderBlock(block);
    if (!rendered.ok) {
      return rendered;
    }
    text_stack << rendered.rml;
    has_text = true;
  }

  if (has_text) {
    result.rml = "<div class=\"stack\">" + text_stack.str() + "</div>";
  }
  return result;
}

std::optional<std::vector<EmbeddedToolCall>> StructuredTextParser::ExtractEmbeddedToolCalls(
    const std::string& llm_output) {
  const auto payload = ExtractJsonPayload(llm_output);
  if (!payload) {
    return std::nullopt;
  }

  nlohmann::json doc = nlohmann::json::parse(*payload, nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("blocks") || !doc["blocks"].is_array()) {
    return std::nullopt;
  }

  std::vector<EmbeddedToolCall> tools;
  bool has_display = false;

  for (const auto& block : doc["blocks"]) {
    if (!block.is_object()) {
      continue;
    }
    if (block.contains("type") && block["type"].is_string() &&
        IsDisplayBlockType(block["type"].get<std::string>())) {
      has_display = true;
      continue;
    }
    if (!IsEmbeddedToolBlock(block)) {
      continue;
    }

    const std::string name = ToolNameFromBlock(block);
    if (name.empty()) {
      continue;
    }
    tools.push_back({name, ToolArgumentsFromBlock(block)});
  }

  if (tools.empty() || has_display) {
    return std::nullopt;
  }
  return tools;
}

ParseResult StructuredTextParser::ParseFromLlmOutput(const std::string& llm_output) {
  if (ExtractEmbeddedToolCalls(llm_output)) {
    return Fail("Response contains tool calls, not display blocks");
  }

  static const std::regex json_re(R"re(```json\s*([\s\S]*?)```)re", std::regex::icase);
  std::smatch match;
  if (std::regex_search(llm_output, match, json_re)) {
    return ParseBlocksJson(TrimAsciiWhitespace(match[1].str()));
  }

  // Tool-calling models often return raw JSON without fences.
  const std::string trimmed = TrimAsciiWhitespace(llm_output);
  if (!trimmed.empty() && trimmed.front() == '{') {
    auto bare = ParseBlocksJson(trimmed);
    if (bare.ok) {
      return bare;
    }
  }

  return Fail("No ```json block found in LLM output");
}

} // namespace pbr
