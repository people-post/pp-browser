// Opening a link (Kenneth 2026-11-04, PLAN-ui-refresh-impl 69/70): links from our own feed (Home's briefs, AI answers'
// articles and sources) go straight to the browser; a link someone sent in a chat asks first, with one fixed question
// and the host only, never the whole address.
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/ChatAnswer.h"

#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace pbr {
namespace {

/** The file with "\r\n" folded to "\n": a Windows checkout has CRLF line ends (core.autocrlf). */
std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  std::string folded;
  folded.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      continue;
    }
    folded += text[i];
  }
  return folded;
}

/** The text of a function from its definition line (`signature` starts a line) to the closing brace at column 0. */
std::optional<std::string> FunctionBody(const std::string& source, const std::string& signature) {
  const size_t at = source.find("\n" + signature);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const size_t close = source.find("\n}\n", at);
  if (close == std::string::npos) {
    return std::nullopt;
  }
  return source.substr(at + 1, close + 2 - (at + 1));
}

std::string ControllerSource() {
  return ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
}

TEST(OpenLinkSourceTest, OwnFeedLinksOpenWithoutAPrompt) {
  const std::string source = ControllerSource();
  ASSERT_FALSE(source.empty()) << "ChatController.cpp not found under PP_SOURCE_DIR";
  // Home's brief and an AI answer's link: the trusted path, never the confirm.
  for (const char* signature : {"void ChatController::OpenHomeBrief(", "void ChatController::OpenChatLinkIn("}) {
    const auto body = FunctionBody(source, signature);
    ASSERT_TRUE(body.has_value()) << signature << " not found";
    EXPECT_NE(body->find("OpenTrustedUrl("), std::string::npos) << signature;
    EXPECT_EQ(body->find("ConfirmAndOpenUrl("), std::string::npos) << signature;
  }
  const std::string header = ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.h");
  EXPECT_NE(header.find("void AskAiOpenUrl(const std::string& url) override { OpenTrustedUrl(url); }"),
            std::string::npos);
  // An article card's button is trusted only when the AI assistant wrote the message.
  const auto action = FunctionBody(source, "void ChatController::HandleLocalActionIn(");
  ASSERT_TRUE(action.has_value());
  EXPECT_NE(action->find("MessageFromAssistant(thread_id, entry_id)"), std::string::npos);
}

TEST(OpenLinkSourceTest, ChatLinksStillAsk) {
  const auto body = FunctionBody(ControllerSource(), "void ChatController::OpenMessageLink(");
  ASSERT_TRUE(body.has_value());
  EXPECT_NE(body->find("ConfirmAndOpenUrl("), std::string::npos);
  EXPECT_EQ(body->find("OpenTrustedUrl("), std::string::npos);
}

TEST(OpenLinkSourceTest, TrustedPathStillOpensHttpsOnly) {
  const auto body = FunctionBody(ControllerSource(), "void ChatController::OpenTrustedUrl(");
  ASSERT_TRUE(body.has_value());
  EXPECT_NE(body->find("IsOpenableUrl(url)"), std::string::npos);
}

TEST(OpenLinkSourceTest, ConfirmShowsTheHostNotTheAddress) {
  const auto body = FunctionBody(ControllerSource(), "void ChatController::ConfirmAndOpenUrl(");
  ASSERT_TRUE(body.has_value());
  EXPECT_NE(body->find("Tr(\"chat.open_link_from\", {{\"host\", UrlHost(url)}})"), std::string::npos);
  EXPECT_EQ(body->find("{\"url\", url}"), std::string::npos) << "the dialog must not carry the full URL";
  EXPECT_NE(body->find("Tr(\"chat.open_link_skip\")"), std::string::npos) << "the checkbox stays";
  EXPECT_NE(body->find("Tr(\"chat.open_link_open\")"), std::string::npos) << "the button says Open";
}

class OpenLinkLocaleTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

INSTANTIATE_TEST_SUITE_P(Locales, OpenLinkLocaleTest, ::testing::Values("en", "zh-Hans"));

TEST_P(OpenLinkLocaleTest, DialogTextIsFixedAndShowsTheHostOnly) {
  const bool en = std::string(GetParam()) == "en";
  EXPECT_EQ(Tr("chat.open_link_title"), en ? "Open in browser?" : "在浏览器中打开？");
  EXPECT_EQ(Tr("chat.open_link_open"), en ? "Open" : "打开");
  const std::string url = "https://news.example.com/2026/11/04/a-long-article?utm_source=x#top";
  const std::string line = Tr("chat.open_link_from", {{"host", UrlHost(url)}});
  EXPECT_EQ(line, en ? "From news.example.com" : "来自 news.example.com");
  EXPECT_EQ(line.find("/2026/"), std::string::npos);
  EXPECT_EQ(line.find("https://"), std::string::npos);
}

} // namespace
} // namespace pbr
