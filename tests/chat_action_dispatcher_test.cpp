#include "base/ai/StructuredTextParser.h"

#include <gtest/gtest.h>

#include <string>

TEST(ChatActionDispatcherTest, ParsesMultiButtonBlocksWithOrderedIndices) {
  const std::string multi_button = R"({
    "blocks": [
      { "type": "paragraph", "text": "Pick one:" },
      { "type": "button", "label": "Alpha", "message": "alpha" },
      { "type": "button", "label": "Beta", "message": "beta" },
      { "type": "button", "label": "Gamma", "message": "gamma" }
    ]
  })";

  auto result = pbr::StructuredTextParser::ParseBlocksJson(multi_button);
  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.chat_actions.size(), 3u);
  EXPECT_EQ(result.chat_actions[0].label, "Alpha");
  EXPECT_EQ(result.chat_actions[1].label, "Beta");
  EXPECT_EQ(result.chat_actions[2].label, "Gamma");
  EXPECT_NE(result.rml.find("send_chat_action('__ENTRY__', 0)"), std::string::npos);
  EXPECT_NE(result.rml.find("send_chat_action('__ENTRY__', 1)"), std::string::npos);
  EXPECT_NE(result.rml.find("send_chat_action('__ENTRY__', 2)"), std::string::npos);

  const size_t pos0 = result.rml.find("send_chat_action('__ENTRY__', 0)");
  const size_t pos1 = result.rml.find("send_chat_action('__ENTRY__', 1)");
  const size_t pos2 = result.rml.find("send_chat_action('__ENTRY__', 2)");
  EXPECT_LT(pos0, pos1);
  EXPECT_LT(pos1, pos2);
}
