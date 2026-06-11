#include "agent/StructuredTextParser.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  const std::string valid = R"({
    "blocks": [
      { "type": "paragraph", "text": "Hello <world> & \"quotes\"" },
      { "type": "heading", "level": 2, "text": "Title" },
      { "type": "list", "ordered": false, "items": ["A", "B"] },
      { "type": "code", "text": "int x = 1;" }
    ]
  })";

  auto result = pbr::StructuredTextParser::ParseBlocksJson(valid);
  assert(result.ok);
  assert(result.rml.find("assistant-message") == std::string::npos);
  assert(result.rml.find("selectable=\"text\"") == std::string::npos);
  assert(result.rml.find("<div class=\"stack\">") != std::string::npos);
  assert(result.suggestions.empty());
  assert(result.rml.find("&lt;world&gt;") != std::string::npos);
  assert(result.rml.find("&amp;") != std::string::npos);
  assert(result.rml.find("&quot;") != std::string::npos);
  assert(result.rml.find("<h2>Title</h2>") != std::string::npos);
  assert(result.rml.find("<ul>") != std::string::npos);
  assert(result.rml.find("code-block") != std::string::npos);

  const std::string bad_type = R"({"blocks":[{"type":"table","text":"x"}]})";
  auto bad_type_result = pbr::StructuredTextParser::ParseBlocksJson(bad_type);
  assert(!bad_type_result.ok);
  assert(bad_type_result.rml.find("error") != std::string::npos);

  const std::string bad_json = "not json";
  auto bad_json_result = pbr::StructuredTextParser::ParseBlocksJson(bad_json);
  assert(!bad_json_result.ok);

  const std::string llm_output = "Here is the answer:\n```json\n{\"blocks\":[{\"type\":\"paragraph\",\"text\":\"Hi\"}]}\n```";
  auto llm_result = pbr::StructuredTextParser::ParseFromLlmOutput(llm_output);
  assert(llm_result.ok);
  assert(llm_result.rml.find("<p>Hi</p>") != std::string::npos);

  const std::string heading_clamp = R"({"blocks":[{"type":"heading","level":6,"text":"Big"}]})";
  auto heading_clamp_result = pbr::StructuredTextParser::ParseBlocksJson(heading_clamp);
  assert(!heading_clamp_result.ok);

  const std::string button_block = R"({
    "blocks": [
      { "type": "button", "label": "Say \"hi\"", "message": "It's a \\test" }
    ]
  })";
  auto button_result = pbr::StructuredTextParser::ParseBlocksJson(button_block);
  assert(button_result.ok);
  assert(button_result.rml.empty());
  assert(button_result.suggestions.size() == 1);
  assert(button_result.suggestions[0].label == "Say \"hi\"");
  assert(button_result.suggestions[0].message == "It's a \\test");

  const std::string button_missing_message = R"({"blocks":[{"type":"button","label":"Go"}]})";
  auto button_missing_message_result = pbr::StructuredTextParser::ParseBlocksJson(button_missing_message);
  assert(!button_missing_message_result.ok);

  const std::string button_empty_message = R"({"blocks":[{"type":"button","label":"Go","message":""}]})";
  auto button_empty_message_result = pbr::StructuredTextParser::ParseBlocksJson(button_empty_message);
  assert(!button_empty_message_result.ok);

  // LLMs often omit the final closing brace on the root object.
  const std::string missing_root_brace = R"({
    "blocks": [
      { "type": "paragraph", "text": "Hi" },
      { "type": "button", "label": "More", "message": "Tell me more" }
    ]
  )";
  auto repaired_result = pbr::StructuredTextParser::ParseBlocksJson(missing_root_brace);
  assert(repaired_result.ok);
  assert(repaired_result.rml.find("<p>Hi</p>") != std::string::npos);
  assert(repaired_result.rml.find("selectable=\"text\"") == std::string::npos);
  assert(repaired_result.suggestions.size() == 1);
  assert(repaired_result.suggestions[0].label == "More");
  assert(repaired_result.suggestions[0].message == "Tell me more");

  std::cout << "structured_text_parser_test ok\n";
  return 0;
}
