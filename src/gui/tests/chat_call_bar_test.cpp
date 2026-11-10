// The phone's group call bar (DEV/PLAN-ui-batch8-group-pages.md 4): under the chat header while a call there may be
// joined, its words from the call presenter, and "Join" as a 44dp target around the header's green pill.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(ChatCallBarTest, TheBarSitsUnderTheHeaderAndJoins) {
  const std::string rml = ReadAsset("views/chat.rml");
  const size_t header_end = rml.find("<div class=\"chat-gap-banner");
  const size_t bar = rml.find("<div class=\"chat-call-bar\" data-if=\"call_bar_text != '' && !search_open\">");
  ASSERT_NE(bar, std::string::npos);
  EXPECT_LT(bar, header_end);
  EXPECT_NE(rml.find("data-event-click=\"join_group_call()\"", bar), std::string::npos);
  EXPECT_NE(rml.find("chat-header-call-btn chat-header-call-btn--join\"><span>{{i18n:call.join}}</span>", bar),
            std::string::npos);
  // With the bar up its button is the one "Join": the header's goes back to the plain phone icon.
  const size_t header_btn = rml.find("class=\"chat-header-call-btn\"");
  ASSERT_NE(header_btn, std::string::npos);
  EXPECT_LT(header_btn, bar);
  EXPECT_NE(rml.find("data-class-chat-header-call-btn--join=\"call_joinable && call_bar_text == ''\"", header_btn),
            std::string::npos);
  EXPECT_NE(rml.find("<span data-if=\"call_joinable && call_bar_text == ''\">{{i18n:call.join}}</span>", header_btn),
            std::string::npos);
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t join = css.find(".chat-call-bar-join {");
  ASSERT_NE(join, std::string::npos);
  EXPECT_NE(css.find("min-width: 44dp;\n  height: 44dp;", join), std::string::npos);
  const std::string light = ReadAsset("themes/colors-light.rcss");
  EXPECT_NE(light.find(".chat-call-bar {"), std::string::npos);
}

} // namespace
