#include "agent/StructuredTextParser.h"

#include <nlohmann/json.hpp>
#include <regex>
#include <sstream>

namespace ppbrowser {

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

namespace {

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

} // namespace

ParseResult StructuredTextParser::ParseBlocksJson(const std::string& json) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(json);
  } catch (const nlohmann::json::exception& e) {
    return Fail(std::string("Invalid JSON: ") + e.what());
  }

  if (!doc.is_object() || !doc.contains("blocks") || !doc["blocks"].is_array()) {
    return Fail("JSON must contain a blocks array");
  }

  std::ostringstream out;
  out << "<div class=\"stack\">";
  for (const auto& block : doc["blocks"]) {
    auto rendered = RenderBlock(block);
    if (!rendered.ok) {
      return rendered;
    }
    out << rendered.rml;
  }
  out << "</div>";

  ParseResult result;
  result.ok = true;
  result.rml = out.str();
  return result;
}

ParseResult StructuredTextParser::ParseFromLlmOutput(const std::string& llm_output) {
  static const std::regex json_re(R"re(```json\s*([\s\S]*?)```)re", std::regex::icase);
  std::smatch match;
  if (!std::regex_search(llm_output, match, json_re)) {
    return Fail("No ```json block found in LLM output");
  }
  return ParseBlocksJson(match[1].str());
}

} // namespace ppbrowser
