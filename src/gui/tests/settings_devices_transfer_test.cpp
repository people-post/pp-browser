// Settings > Devices & transfer, layout and wording only (Kenneth 2026-11-14): the "?" after the title, the disabled
// transfer row on a linked phone, and the specificity fix that keeps a desktop-only block off phones (the real
// stylesheets are loaded). Which captions show per device is pinned in settings_row_checklist_test.cpp; every event is
// unchanged, except that the transfer row has none on a linked phone.
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string Asset(const char* relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

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

// --- the real stylesheet ------------------------------------------------------------------------------------------

class DevicesStyleTest : public testing::Test {
protected:
  void TearDown() override {
    if (context) {
      ui::RemoveContext("devices-style-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  ui::ElementDocument* Load(const std::string& body) {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    EXPECT_TRUE(initialized);
    context = ui::CreateContext("devices-style-test", {390, 844});
    EXPECT_NE(context, nullptr);
    context->ActivateTheme("light", true);
    const std::string rml = "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
                            Asset("themes/colors-light.rcss") + "</style></head><body style=\"padding: 0;\">" + body +
                            "</body></rml>";
    ui::ElementDocument* document = context->LoadDocumentFromMemory(rml);
    EXPECT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
    return document;
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
};

TEST_F(DevicesStyleTest, ADesktopOnlyBlockStaysHiddenOnPhonesWhateverElseItCarries) {
  // The leak: .settings-section--grouped .settings-actions (display:block) out-weighed .settings-desktop-only, so the
  // desktop caption under the actions group showed on phones. Every class set the views combine with it is checked.
  const std::vector<std::string> combos = {
      "stack settings-actions settings-desktop-only",
      "settings-toggle-row row group-actions settings-desktop-only",
      "btn settings-desktop-only",
      "btn btn-secondary btn-icon settings-desktop-only",
      "me-profile-actions settings-desktop-only",
      "settings-action-bar settings-desktop-only",
      "group-card settings-desktop-only",
      "text muted settings-help group-foot settings-desktop-only",
  };
  std::string items;
  for (size_t i = 0; i < combos.size(); ++i) {
    items += "<div id=\"d" + std::to_string(i) + "\" class=\"" + combos[i] + "\">x</div>";
  }
  ui::ElementDocument* document =
      Load("<div class=\"shell-layer-compact\"><div class=\"settings-section settings-section--grouped stack\">" +
           items + "<div id=\"shown\" class=\"settings-phone-only\">y</div></div></div>");
  for (size_t i = 0; i < combos.size(); ++i) {
    ui::Element* element = document->GetElementById(("d" + std::to_string(i)).c_str());
    ASSERT_NE(element, nullptr);
    EXPECT_EQ(element->GetComputedValues().display(), ui::Style::Display::None) << combos[i];
  }
  EXPECT_NE(document->GetElementById("shown")->GetComputedValues().display(), ui::Style::Display::None);
}

TEST_F(DevicesStyleTest, TheHelpMarkFollowsTheTitleAndHasA44dpTarget) {
  const std::string title = "Link a new device";
  ui::ElementDocument* document = Load(
      "<div class=\"shell-layer-compact\" style=\"position: absolute; left: 0dp; top: 0dp; width: 390dp;\">"
      "<div class=\"settings-section settings-section--grouped settings-mock stack\"><div class=\"group-card\">"
      "<div class=\"group-row\"><span class=\"group-row-body\"><span class=\"settings-q-label\">"
      "<span id=\"t\" class=\"group-row-text\">" +
      title +
      "</span>"
      "<button id=\"q\" class=\"settings-q-btn\" type=\"button\"></button></span>"
      "<span id=\"chev\" style=\"width:16dp;height:16dp\"></span></span></div></div></div></div>");
  ui::Element* text = document->GetElementById("t");
  ui::Element* q = document->GetElementById("q");
  ui::Element* chevron = document->GetElementById("chev");
  const float text_right =
      text->GetAbsoluteOffset(ui::BoxArea::Border).x + text->GetBox().GetSize(ui::BoxArea::Border).x;
  const float q_left = q->GetAbsoluteOffset(ui::BoxArea::Border).x;
  // The text is its own width (not stretched), and the 44dp target starts at the text's end (a 6dp negative margin takes
  // the 24dp of air round the 20dp glyph down to about 4dp of visible gap).
  EXPECT_LT(text_right, 200.f);
  EXPECT_NEAR(q_left, text_right - 6.f, 1.f);
  EXPECT_FLOAT_EQ(q->GetBox().GetSize(ui::BoxArea::Border).x, 44.f);
  EXPECT_FLOAT_EQ(q->GetBox().GetSize(ui::BoxArea::Border).y, 44.f);
  EXPECT_LT(q_left + 44.f, chevron->GetAbsoluteOffset(ui::BoxArea::Border).x);
}

TEST_F(DevicesStyleTest, AGroupedPagePrimaryButtonIsFullWidthAnd50dp) {
  ui::ElementDocument* document = Load(
      "<div class=\"shell-layer-compact\" style=\"position: absolute; left: 0dp; top: 0dp; width: 390dp;\">"
      "<div class=\"settings-section settings-section--grouped settings-mock stack\">"
      "<button id=\"b\" class=\"btn btn-primary settings-page-btn\" type=\"button\">Copy link code</button>"
      "</div></div>");
  ui::Element* button = document->GetElementById("b");
  const ui::Vector2f size = button->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(size.y, 50.f);
  EXPECT_FLOAT_EQ(size.x, 390.f - 32.f) << "16dp gutters";
}

TEST_F(DevicesStyleTest, TheHeroButtonIs50dpTheHelpMarkIs44dpAndTheSentenceStaysOnTheScreen) {
  ui::ElementDocument* document = Load(
      "<div class=\"shell-layer-compact\" style=\"position: absolute; left: 0dp; top: 0dp; width: 390dp;\">"
      "<div class=\"settings-section settings-section--grouped settings-mock stack\">"
      "<div class=\"settings-phone-only settings-dev-hero\">"
      "<div id=\"row\" class=\"settings-dev-sentence-row\"><span id=\"s\" "
      "class=\"settings-dev-sentence\">在其他设备上使用同一个账号</span>"
      "<button id=\"q\" class=\"settings-q-btn\" type=\"button\"></button></div>"
      "<p class=\"text muted settings-dev-sub\">最多 5 台。新设备上看不到以前的聊天记录。</p>"
      "<button id=\"b\" class=\"btn btn-primary\" type=\"button\">关联新设备</button></div>"
      "<div class=\"group-card\"><div id=\"r\" class=\"group-row group-row--static\"><span class=\"group-row-body\">"
      "<span class=\"group-row-label\"><span class=\"group-row-text\">这台手机</span></span>"
      "<span class=\"group-row-value\">本机 · 主设备</span></span></div></div></div></div>");
  const ui::Vector2f button = document->GetElementById("b")->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(button.y, 50.f);
  EXPECT_FLOAT_EQ(button.x, 390.f - 32.f);
  const ui::Vector2f q = document->GetElementById("q")->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(q.x, 44.f);
  EXPECT_FLOAT_EQ(q.y, 44.f);
  ui::Element* q_element = document->GetElementById("q");
  EXPECT_LE(q_element->GetAbsoluteOffset(ui::BoxArea::Border).x + 44.f, 390.f);
  EXPECT_GE(document->GetElementById("r")->GetBox().GetSize(ui::BoxArea::Border).y, 50.f);
}

// --- the markup ---------------------------------------------------------------------------------------------------

TEST(DevicesTransferMarkupTest, TheNewCaptionsExistInBothLanguagesAndReadAsAgreed) {
  const std::string zh = Asset("locales/zh-Hans.json");
  const std::string en = Asset("locales/en.json");
  EXPECT_NE(zh.find("\"关联新设备、移除设备，要在主设备上操作。\""), std::string::npos);
  EXPECT_NE(en.find("\"Linking a new device or removing one is done on the main device.\""), std::string::npos);
  EXPECT_NE(en.find("\"Use the same account on other devices\""), std::string::npos);
  EXPECT_NE(en.find("\"No other devices linked yet.\""), std::string::npos);
  EXPECT_NE(en.find("\"Changing phones: link the new phone, make it the main device, then remove the old one.\""),
            std::string::npos);
}

TEST(DevicesTransferMarkupTest, TheSaveCopyRowAndTheHeroSentenceCarryTheHelpMarkInline) {
  const std::string rml = Asset("views/settings_section_devices.rml");
  const size_t at = rml.find("<span class=\"group-row-text\">{{i18n:settings.security.copy.save}}");
  ASSERT_NE(at, std::string::npos);
  const size_t label = rml.rfind("<span class=\"settings-q-label\">", at);
  ASSERT_NE(label, std::string::npos);
  EXPECT_LT(at - label, 40u) << "the title is the first child of the inline label";
  EXPECT_NE(rml.find("class=\"settings-q-btn\"", at), std::string::npos);
  // The action's "?" follows the sentence directly and asks about the action's own title.
  const size_t sentence = rml.find("{{i18n:settings.security.devices.hero}}</span><button class=\"settings-q-btn\"");
  ASSERT_NE(sentence, std::string::npos);
  EXPECT_NE(rml.find("ask_setting_help('settings.security.transfer_row')", sentence), std::string::npos);
  EXPECT_EQ(rml.find("ask_setting_help('settings.security.transfer_row')",
                     rml.find("ask_setting_help('settings.security.transfer_row')") + 1),
            std::string::npos)
      << "one \"?\" for the action";
  EXPECT_EQ(rml.find("group-row-q"), std::string::npos) << "no absolutely positioned help mark any more";
  EXPECT_EQ(Asset("themes/components.rcss").find("group-row-q"), std::string::npos);
  EXPECT_EQ(rml.find("group-row-tap"), std::string::npos);
}

TEST(DevicesTransferMarkupTest, NoDisabledLinkRowIsLeftAndTheLinkedPhonesBlockHasNothingToTap) {
  const std::string rml = Asset("views/settings_section_devices.rml");
  EXPECT_EQ(rml.find("settings.security.transfer_off"), std::string::npos);
  EXPECT_EQ(rml.find("group-row--off\" data-if"), std::string::npos);
  const size_t from = rml.find("data-if=\"own_devices_not_main && !own_devices_signed_out\"");
  ASSERT_NE(from, std::string::npos);
  const size_t to = rml.find("<div class=\"stack settings-actions\" data-if=\"own_devices_shown\">", from);
  ASSERT_NE(to, std::string::npos);
  const std::string block = rml.substr(from, to - from);
  EXPECT_EQ(block.find("<button"), std::string::npos);
  EXPECT_EQ(block.find("data-event-"), std::string::npos);
  EXPECT_EQ(block.find("chevron"), std::string::npos);
}

TEST(DevicesTransferMarkupTest, TheHelpQuestionOfTheLinkActionReadsRightWithTheNewTitle) {
  const std::string zh = Asset("locales/zh-Hans.json");
  const std::string en = Asset("locales/en.json");
  EXPECT_NE(zh.find("\"settings.help.question\": \"「{title}」是什么意思？\""), std::string::npos);
  EXPECT_NE(zh.find("\"settings.security.transfer_row\": \"关联新设备\""), std::string::npos);
  EXPECT_NE(en.find("\"settings.help.question\": \"What does “{title}” do?\""), std::string::npos);
  EXPECT_NE(en.find("\"settings.security.transfer_row\": \"Link a new device\""), std::string::npos);
}

} // namespace
} // namespace pbr
