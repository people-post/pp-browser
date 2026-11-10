// TODO #205: in an AI conversation and the ask-AI panel there is no microphone (no speech-to-text yet), so the right
// round slot of the composer is always Send: drawn disabled while the field is empty. Markup first, then the geometry
// with the real stylesheets.
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {64, 64};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string Asset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

/** The idle Send button element (from its opening tag to </button>) of a view. */
std::string IdleButton(const std::string& rml) {
  const size_t at = rml.find("prompt-send-btn prompt-send-btn--idle");
  if (at == std::string::npos) {
    return {};
  }
  const size_t begin = rml.rfind("<button", at);
  return rml.substr(begin, rml.find("</button>", at) + 9 - begin);
}

TEST(AiComposerSendSlotMarkup, ChatComposerHasADisabledSendForAiThreadsOnly) {
  const std::string button = IdleButton(Asset("views/composer.rml"));
  ASSERT_FALSE(button.empty());
  EXPECT_NE(button.find("disabled"), std::string::npos);
  EXPECT_EQ(button.find("send_message()"), std::string::npos) << "not tappable";
  EXPECT_NE(button.find("!thread_is_private"), std::string::npos) << "human chats keep the mic <-> Send switch";
  EXPECT_NE(button.find("!thread_is_group"), std::string::npos);
  EXPECT_NE(button.find("draft == ''"), std::string::npos);
}

TEST(AiComposerSendSlotMarkup, AskAiPanelHasADisabledSendWhileTheFieldIsEmpty) {
  const std::string button = IdleButton(Asset("views/ask_ai.rml"));
  ASSERT_FALSE(button.empty());
  EXPECT_NE(button.find("disabled"), std::string::npos);
  EXPECT_EQ(button.find("ask_ai_send()"), std::string::npos) << "not tappable";
  EXPECT_NE(button.find("draft == ''"), std::string::npos);
}

TEST(AiComposerSendSlotMarkup, DisabledFillIsAThemeToken) {
  const std::string tokens = Asset("themes/theme_tokens.tsv");
  EXPECT_NE(tokens.find("x-send-disabled\t#c7c7cc\t#48484a"), std::string::npos);
  EXPECT_NE(Asset("themes/colors.rcss.tmpl").find("{{x-send-disabled}}"), std::string::npos);
  EXPECT_NE(Asset("themes/colors-light.rcss").find("#c7c7cc"), std::string::npos);
  EXPECT_NE(Asset("themes/colors-dark.rcss").find("#48484a"), std::string::npos);
}

class AiComposerSendSlotGeometry : public testing::TestWithParam<const char*> {
protected:
  void TearDown() override {
    if (context) {
      ui::RemoveContext("ai-send-slot-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
};

// The slot is a 44dp round button at the row's right end in both contexts (same place the accent Send takes).
TEST_P(AiComposerSendSlotGeometry, TheRightSlotIsOccupiedByA44dpRoundButton) {
  ui::SetRenderInterface(&renderer);
  initialized = ui::Initialise();
  ASSERT_TRUE(initialized);
  context = ui::CreateContext("ai-send-slot-test", {390, 844});
  ASSERT_NE(context, nullptr);
  context->ActivateTheme("light", true);
  const std::string view = std::string(GetParam()) == "ask-ai" ? "views/ask_ai.rml" : "views/composer.rml";
  std::string button = IdleButton(Asset(view.c_str()));
  button = std::regex_replace(button, std::regex("\\s+data-[a-z-]+=\"[^\"]*\""), "");
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/colors-light.rcss") +
      "</style></head><body style=\"padding: 0;\">"
      "<div class=\"shell-layer-compact\" style=\"position: absolute; left: 0dp; top: 600dp; width: 390dp;\">"
      "<div class=\"" +
      std::string(GetParam()) +
      "\"><div class=\"prompt-composer-row\" style=\"position: relative; height: 46dp;\">"
      "<div class=\"prompt-composer-pill\"></div>" +
      button + "</div></div></div></body></rml>";
  ui::ElementDocument* document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Update();
  ui::ElementList found;
  document->GetElementsByClassName(found, "prompt-send-btn--idle");
  ASSERT_FALSE(found.empty());
  ui::Element* send = found.front();
  EXPECT_NE(send->GetComputedValues().display(), ui::Style::Display::None);
  const ui::Vector2f size = send->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(size.x, 44.f);
  EXPECT_FLOAT_EQ(size.y, 44.f);
  EXPECT_FLOAT_EQ(send->GetAbsoluteOffset(ui::BoxArea::Border).x + size.x, 390.f) << "at the row's right end";
}

INSTANTIATE_TEST_SUITE_P(Contexts, AiComposerSendSlotGeometry, testing::Values("shell-chat-overlay", "ask-ai"));

} // namespace
} // namespace pbr
