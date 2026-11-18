// "Ask me anything, or open Conversations to continue" is the AI thread's empty state: a chat with a person or a group
// that has no messages yet does not show it.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(ChatEmptyCardTest, TheAiEmptyStateShowsOnlyInAnAiThread) {
  const std::string rml = ReadAsset("views/chat.rml");
  const size_t body = rml.find("{{i18n:chat.empty_body}}");
  ASSERT_NE(body, std::string::npos);
  const size_t open = rml.rfind("<div class=\"chat-empty", body);
  ASSERT_NE(open, std::string::npos);
  const std::string tag = rml.substr(open, rml.find('>', open) - open);
  EXPECT_NE(tag.find("thread_is_ai"), std::string::npos);
  EXPECT_NE(tag.find("!has_turns"), std::string::npos);
}

} // namespace
} // namespace pbr
