#include "base/ai/StructuredTextParser.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  const std::string multi_button = R"({
    "blocks": [
      { "type": "paragraph", "text": "Pick one:" },
      { "type": "button", "label": "Alpha", "message": "alpha" },
      { "type": "button", "label": "Beta", "message": "beta" },
      { "type": "button", "label": "Gamma", "message": "gamma" }
    ]
  })";

  auto result = pbr::StructuredTextParser::ParseBlocksJson(multi_button);
  assert(result.ok);
  assert(result.chat_actions.size() == 3);
  assert(result.chat_actions[0].label == "Alpha");
  assert(result.chat_actions[1].label == "Beta");
  assert(result.chat_actions[2].label == "Gamma");
  assert(result.rml.find("send_chat_action('__ENTRY__', 0)") != std::string::npos);
  assert(result.rml.find("send_chat_action('__ENTRY__', 1)") != std::string::npos);
  assert(result.rml.find("send_chat_action('__ENTRY__', 2)") != std::string::npos);

  const size_t pos0 = result.rml.find("send_chat_action('__ENTRY__', 0)");
  const size_t pos1 = result.rml.find("send_chat_action('__ENTRY__', 1)");
  const size_t pos2 = result.rml.find("send_chat_action('__ENTRY__', 2)");
  assert(pos0 < pos1);
  assert(pos1 < pos2);

  std::cout << "chat_action_dispatcher_test ok\n";
  return 0;
}
