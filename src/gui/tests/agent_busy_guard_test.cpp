// Guards for the per-thread busy change (B112): the send paths must not read the global `chat_.loading` again,
// and the queue's wording must exist in both languages.
#include "foundation/i18n/LocalizationService.h"

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

TEST(AgentBusySourceTest, SendPathsDoNotReadTheGlobalLoadingFlag) {
  const std::string source = ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  ASSERT_FALSE(source.empty()) << "ChatController.cpp not found under PP_SOURCE_DIR";
  for (const char* signature : {"void ChatController::SendUserText(", "void ChatController::OnSendMessage("}) {
    const auto body = FunctionBody(source, signature);
    ASSERT_TRUE(body.has_value()) << signature << " not found";
    EXPECT_GT(body->size(), 200u) << signature << ": the extracted body looks too short";
    EXPECT_EQ(body->find("chat_.loading"), std::string::npos)
        << signature << " reads chat_.loading: use DecideSend / the per-thread busy state instead";
  }
}

class AgentQueueLocaleTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

INSTANTIATE_TEST_SUITE_P(Locales, AgentQueueLocaleTest, ::testing::Values("en", "zh-Hans"));

TEST_P(AgentQueueLocaleTest, QueueWordingExists) {
  for (const char* key : {"chat.ai_queue.pending", "chat.ai_queue.cancel", "chat.ai_queue.already",
                          "chat.ai_queue.image_busy", "chat.ai_busy_elsewhere"}) {
    const std::string text = Tr(key);
    EXPECT_NE(text, key) << key << " is missing in " << GetParam();
    EXPECT_FALSE(text.empty()) << key << " is empty in " << GetParam();
  }
}

} // namespace
} // namespace pbr
