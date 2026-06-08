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

  auto result = ppbrowser::StructuredTextParser::ParseBlocksJson(valid);
  assert(result.ok);
  assert(result.rml.find("&lt;world&gt;") != std::string::npos);
  assert(result.rml.find("&amp;") != std::string::npos);
  assert(result.rml.find("&quot;") != std::string::npos);
  assert(result.rml.find("<h2>Title</h2>") != std::string::npos);
  assert(result.rml.find("<ul>") != std::string::npos);
  assert(result.rml.find("code-block") != std::string::npos);

  const std::string bad_type = R"({"blocks":[{"type":"table","text":"x"}]})";
  auto bad_type_result = ppbrowser::StructuredTextParser::ParseBlocksJson(bad_type);
  assert(!bad_type_result.ok);
  assert(bad_type_result.rml.find("error") != std::string::npos);

  const std::string bad_json = "not json";
  auto bad_json_result = ppbrowser::StructuredTextParser::ParseBlocksJson(bad_json);
  assert(!bad_json_result.ok);

  const std::string llm_output = "Here is the answer:\n```json\n{\"blocks\":[{\"type\":\"paragraph\",\"text\":\"Hi\"}]}\n```";
  auto llm_result = ppbrowser::StructuredTextParser::ParseFromLlmOutput(llm_output);
  assert(llm_result.ok);
  assert(llm_result.rml.find("<p>Hi</p>") != std::string::npos);

  std::cout << "structured_text_parser_test ok\n";
  return 0;
}
