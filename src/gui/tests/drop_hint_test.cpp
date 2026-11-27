// DEV/PLAN-desktop-drop-paste.md, C: the hint over the chat while files are dragged onto it. The shell fields (Bind /
// Dirty / set), the overlay in both desktop chat layouts (and in no other pane), and its real styles: the words show
// when `drop_hint_visible` is true, not when false, centred on the chat, and it never takes a click.
#include "feature/presenters/PictureSendQueue.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/DropPane.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellHost.h"
#include "gui/shell/ShellLayoutRml.h"
#include "lib/ui/ShellLayout.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <cmath>
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
    dimensions = {24, 24};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string Asset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool Has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

ShellState DesktopChat(const LayoutMode mode) {
  ShellState state;
  state.layout_mode = mode;
  state.nav_tab = NavTab::Sessions;
  if (mode == LayoutMode::Compact) {
    state.compact_chat_open = true;
  } else {
    state.panes.push_back({.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
    state.primary_pane_key = "chat";
  }
  return state;
}

TEST(DropHintMarkupTest, TheWideChatPaneAndTheNarrowChatPageEachCarryTheHint) {
  const std::string hint = ShellLayoutRml::DropHintRml();
  ASSERT_TRUE(Has(hint, "data-if=\"drop_hint_visible\""));
  ASSERT_TRUE(Has(hint, "{{drop_hint_text}}"));

  const std::string wide = ShellLayoutRml(DesktopChat(LayoutMode::Expanded), false, "").ShellRoot();
  const size_t pane = wide.find("id=\"pane-chat\"");
  const size_t at = wide.find(hint);
  ASSERT_NE(pane, std::string::npos);
  ASSERT_NE(at, std::string::npos) << "the wide chat pane has the hint";
  EXPECT_GT(at, pane);
  EXPECT_LT(at, wide.find("id=\"pane-composer-chat\"")) << "inside the pane, before its composer strip";
  EXPECT_EQ(wide.find(hint, at + 1), std::string::npos) << "one";

  const std::string narrow = ShellLayoutRml(DesktopChat(LayoutMode::Compact), false, "").ShellRoot();
  const size_t overlay = narrow.find("id=\"shell-chat-overlay\"");
  ASSERT_NE(overlay, std::string::npos);
  ASSERT_NE(narrow.find(hint), std::string::npos) << "the narrow chat page has the hint";
  EXPECT_GT(narrow.find(hint), overlay);
}

TEST(DropHintMarkupTest, NoOtherPaneHasIt) {
  ShellState state;
  state.layout_mode = LayoutMode::Expanded;
  state.nav_tab = NavTab::Contacts;
  state.panes.push_back({.spec = {.key = "contact_detail", .rml_path = "views/contact_detail.rml"}, .id = 1});
  state.primary_pane_key = "contact_detail";
  EXPECT_FALSE(Has(ShellLayoutRml(state, false, "").ShellRoot(), "drop-hint"));
  ShellState list_only;
  list_only.layout_mode = LayoutMode::Compact;
  list_only.nav_tab = NavTab::Sessions;
  EXPECT_FALSE(Has(ShellLayoutRml(list_only, false, "").ShellRoot(), "drop-hint")) << "the list shows, no chat open";
}

TEST(DropHintShellTest, TheShellKeepsAndClearsTheHintText) {
  ShellHost host;
  EXPECT_FALSE(host.State().drop_hint_visible);
  host.SetDropHint("Drop to send to Ada", NameWords{"Drop to send to ", "Ada", ""});
  EXPECT_TRUE(host.State().drop_hint_visible);
  EXPECT_TRUE(host.State().drop_hint_can);
  EXPECT_EQ(std::string(host.State().drop_hint_prefix.c_str()), "Drop to send to ");
  EXPECT_EQ(std::string(host.State().drop_hint_name.c_str()), "Ada");
  EXPECT_TRUE(host.State().drop_hint_suffix.empty());
  EXPECT_EQ(std::string(host.State().drop_hint_text.c_str()), "Drop to send to Ada");
  host.SetDropHint("Files can't be sent here"); // the refusal's own line replaces it
  EXPECT_EQ(std::string(host.State().drop_hint_text.c_str()), "Files can't be sent here");
  EXPECT_FALSE(host.State().drop_hint_can);
  host.SetDropHint("");
  EXPECT_FALSE(host.State().drop_hint_visible);
  EXPECT_TRUE(host.State().drop_hint_text.empty());
  EXPECT_TRUE(host.State().drop_hint_name.empty());
}

// The sentence is cut where the translator put {name}, in either language; the chat controller does this for the hint.
TEST(DropHintWordsTest, TheSentenceIsCutAtTheNameInBothLanguagesAndWhereverTheNameFalls) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  const std::string marker(1, kNameMarker);
  LocalizationService::Instance().SetPreferredLanguage("en");
  NameWords en = SplitAtNameMarker(LocalizationService::Instance().Tr("chat.drop.hint", {{"name", marker}}), "Ada");
  EXPECT_EQ(en.prefix, "Drop to send to ");
  EXPECT_EQ(en.name, "Ada");
  EXPECT_EQ(en.suffix, "");
  EXPECT_EQ(en.prefix + en.name + en.suffix, LocalizationService::Instance().Tr("chat.drop.hint", {{"name", "Ada"}}));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  NameWords zh = SplitAtNameMarker(LocalizationService::Instance().Tr("chat.drop.hint", {{"name", marker}}), "Ada");
  EXPECT_EQ(zh.prefix, "\xE6\x9D\xBE\xE5\xBC\x80\xE5\x8F\x91\xE9\x80\x81\xE7\xBB\x99 ");
  EXPECT_EQ(zh.name, "Ada");
  EXPECT_EQ(zh.suffix, "");
  EXPECT_FALSE(LocalizationService::Instance().Tr("chat.drop.hint_limit").empty());
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(LocalizationService::Instance().Tr("chat.drop.hint_limit"), "Up to 9 at a time");
  EXPECT_EQ(PictureSendQueue::kMaxItems, 9U) << "the limit line says 9: change both together";
  const NameWords middle = SplitAtNameMarker("To " + marker + " now", "Ada");
  EXPECT_EQ(middle.prefix, "To ");
  EXPECT_EQ(middle.suffix, " now");
  const NameWords none = SplitAtNameMarker("No name here", "Ada");
  EXPECT_EQ(none.prefix, "No name here");
  EXPECT_TRUE(none.name.empty());
}

TEST(DropHintShellTest, TheWakeForTheHintsSecondIsArmedByNotifyFrameEnd) {
  ShellHost host;
  host.BindDropHintWake([]() { return std::optional<int64_t>(700); });
  ui::SetRenderInterface(nullptr);
  NullRender renderer;
  ui::SetRenderInterface(&renderer);
  ASSERT_TRUE(ui::Initialise());
  ui::Context* context = ui::CreateContext("drop-hint-wake-test", {400, 300});
  ASSERT_NE(context, nullptr);
  context->Update(); // resets the next-update timeout to "never"
  host.NotifyFrameEnd(context);
  EXPECT_LE(context->GetNextUpdateDelay(), 0.7 + 1e-6);
  EXPECT_GT(context->GetNextUpdateDelay(), 0.0);
  host.BindDropHintWake({});
  context->Update();
  host.NotifyFrameEnd(context);
  EXPECT_GT(context->GetNextUpdateDelay(), 60.0) << "with no hint pending the shell asks for no wake of its own";
  ui::RemoveContext("drop-hint-wake-test");
  ui::Shutdown();
  ui::SetRenderInterface(nullptr);
}

constexpr const char* kContext = "drop-hint-render-test";

class DropHintRenderTest : public testing::TestWithParam<LayoutMode> {
protected:
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext(kContext);
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void Load() {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    for (const char* file : {"fonts/LatoLatin-Regular.ttf", "fonts/Inter-Regular.ttf", "fonts/Inter-Bold.ttf"}) {
      ASSERT_TRUE(ui::LoadFontFace((std::filesystem::path(PP_BROWSER_ASSETS_DIR) / file).string()));
    }
    context = ui::CreateContext(kContext, {1000, 700});
    ASSERT_NE(context, nullptr);
    context->ActivateTheme("light", true);
    ASSERT_TRUE(host.RegisterWindowModel(context));

    // The real layout markup with only the window model's `drop_hint_visible` left bound: everything else in it is
    // another model's and is not under test here.
    std::string root = ShellLayoutRml(DesktopChat(GetParam()), false, "").ShellRoot();
    root = std::regex_replace(
        root, std::regex("\\s+data-(?!model=\"window\"|[a-z-]+=\"[^\"]*drop_hint)[a-z-]+=\"[^\"]*\""), "");
    root = std::regex_replace(root, std::regex("\\{\\{(?!drop_hint_)[^}]*\\}\\}"), ""); // other models' words
    const bool wide = GetParam() == LayoutMode::Expanded;
    const std::string rml = "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
                            (wide ? Asset("themes/wide.rcss") : std::string()) + Asset("themes/colors-light.rcss") +
                            "#frame { position: absolute; top: 0dp; left: 0dp; width: 1000dp; height: 700dp; }"
                            ".shell-layer { position: absolute; top: 0dp; left: 0dp; right: 0dp; bottom: 0dp; }"
                            ".shell-pane-row { display: flex; flex-direction: row; width: 100%; height: 100%; }"
                            ".shell-pane-chat { flex: 1; }"
                            "</style></head><body><div id=\"frame\" class=\"" +
                            std::string(wide ? "shell-body--wide" : "shell-body--compact") + "\">" + root +
                            "</div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }

  ui::Element* Pane() {
    return document->GetElementById(GetParam() == LayoutMode::Expanded ? "pane-chat" : "shell-chat-overlay");
  }
  ui::Element* Hint() { return document->GetElementById("drop-hint"); }
  ui::Element* ByClass(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  }
  /** The words that show: the "can drop" line (prefix, name, suffix) or the refusal's. */
  ui::Element* Label() {
    return ByClass(Shown(ByClass("drop-hint-label--no")) ? "drop-hint-label--no" : "drop-hint-label");
  }
  static bool Shown(ui::Element* element) {
    for (ui::Element* at = element; at != nullptr; at = at->GetParentNode()) {
      if (at->GetComputedValues().display() == ui::Style::Display::None) {
        return false;
      }
    }
    return element != nullptr;
  }
  static ui::Vector2f SizeOf(ui::Element* element) { return element->GetBox().GetSize(ui::BoxArea::Border); }
  static ui::Vector2f AtOf(ui::Element* element) { return element->GetAbsoluteOffset(ui::BoxArea::Border); }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ShellHost host;
};

TEST_P(DropHintRenderTest, TheWordsShowWhileTheHintIsOnAndNotWhenItIsOff) {
  Load();
  ASSERT_NE(Hint(), nullptr);
  EXPECT_FALSE(Hint()->IsVisible()) << "off at first";

  host.SetDropHint("Drop to send to Ada", NameWords{"Drop to send to ", "Ada", ""});
  context->Update();
  ASSERT_TRUE(Hint()->IsVisible());
  ASSERT_NE(Label(), nullptr);
  EXPECT_NE(std::string(Label()->GetInnerRML().c_str()).find("Ada"), std::string::npos);
  EXPECT_GT(Label()->GetBox().GetSize(ui::BoxArea::Border).x, 100.f) << "readable, not collapsed";

  host.SetDropHint("");
  context->Update();
  EXPECT_FALSE(Hint()->IsVisible());
}

TEST_P(DropHintRenderTest, TheHintCoversTheChatCentresItsLabelAndTakesNoClick) {
  Load();
  host.SetDropHint("Drop to send to Ada", NameWords{"Drop to send to ", "Ada", ""});
  context->Update();
  ui::Element* pane = Pane();
  ui::Element* hint = Hint();
  ui::Element* label = Label();
  ASSERT_TRUE(pane && hint && label);
  const ui::Vector2f pane_at = pane->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f pane_size = pane->GetBox().GetSize(ui::BoxArea::Border);
  const ui::Vector2f hint_at = hint->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f hint_size = hint->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_NEAR(hint_at.x, pane_at.x, 0.5f);
  EXPECT_NEAR(hint_at.y, pane_at.y, 0.5f);
  EXPECT_NEAR(hint_size.x, pane_size.x, 0.5f);
  EXPECT_NEAR(hint_size.y, pane_size.y, 0.5f) << "the whole chat";
  const ui::Vector2f label_at = label->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f label_size = label->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_NEAR(label_at.x + label_size.x * 0.5f, hint_at.x + hint_size.x * 0.5f, 1.f) << "the words are centred across";
  // The stack (badge, words, limit) is centred as a whole, down as well.
  ui::Element* stack = ByClass("drop-hint-stack");
  ASSERT_NE(stack, nullptr);
  EXPECT_NEAR(AtOf(stack).y + SizeOf(stack).y * 0.5f, hint_at.y + hint_size.y * 0.5f, 1.f) << "and the stack down";
  EXPECT_EQ(hint->GetComputedValues().pointer_events(), ui::Style::PointerEvents::None);
  EXPECT_NEAR(label->GetComputedValues().font_size(), 17.f, 0.5f);
}

TEST_P(DropHintRenderTest, WhenDroppingWouldSendThereIsTheBadgeTheNameInBoldAndTheLimit) {
  Load();
  host.SetDropHint("Drop to send to Ada", NameWords{"Drop to send to ", "Ada", ""});
  context->Update();
  EXPECT_FALSE(Hint()->IsClassSet("drop-hint--no"));
  ui::Element* badge = ByClass("drop-hint-badge");
  ui::Element* prefix = ByClass("drop-hint-prefix");
  ui::Element* name = ByClass("drop-hint-name");
  ui::Element* limit = ByClass("drop-hint-limit");
  ASSERT_TRUE(Shown(badge) && Shown(prefix) && Shown(name) && Shown(limit));
  EXPECT_NEAR(SizeOf(badge).x, 56.f, 0.5f) << "a 56dp round badge";
  EXPECT_NEAR(SizeOf(badge).y, 56.f, 0.5f);
  EXPECT_NEAR(badge->GetComputedValues().border_top_left_radius(), 28.f, 0.5f);
  EXPECT_EQ(std::string(prefix->GetInnerRML().c_str()), "Drop to send to ");
  EXPECT_EQ(std::string(name->GetInnerRML().c_str()), "Ada");
  EXPECT_GT(name->GetComputedValues().font_weight(), prefix->GetComputedValues().font_weight())
      << "the name is bolder than the rest";
  EXPECT_GT(AtOf(name).x, AtOf(prefix).x + SizeOf(prefix).x - 0.5f) << "the name follows the words, a space between";
  EXPECT_EQ(std::string(limit->GetInnerRML().c_str()), "Up to 9 at a time");
  EXPECT_NEAR(limit->GetComputedValues().font_size(), 13.f, 0.5f);
  EXPECT_FALSE(Shown(ByClass("drop-hint-label--no"))) << "the refusal's line is not shown";
  EXPECT_GT(AtOf(limit).y, AtOf(name).y + SizeOf(name).y - 0.5f) << "the limit is under the words";
  EXPECT_GT(AtOf(name).y, AtOf(badge).y + SizeOf(badge).y - 0.5f) << "the words are under the badge";
}

TEST_P(DropHintRenderTest, ANameInTheMiddleOfTheSentenceIsStillBoldAndInPlace) {
  Load();
  host.SetDropHint("给 Ada 发送", NameWords{"给 ", "Ada", " 发送"});
  context->Update();
  ui::Element* prefix = ByClass("drop-hint-prefix");
  ui::Element* name = ByClass("drop-hint-name");
  ui::Element* suffix = ByClass("drop-hint-suffix");
  ASSERT_TRUE(Shown(prefix) && Shown(name) && Shown(suffix));
  EXPECT_LT(AtOf(prefix).x, AtOf(name).x);
  EXPECT_LT(AtOf(name).x, AtOf(suffix).x) << "the name is not last";
  EXPECT_EQ(std::string(name->GetInnerRML().c_str()), "Ada");
}

TEST_P(DropHintRenderTest, ARefusalHasTheSameShapeInGreyWithItsOwnLineAndNoLimit) {
  Load();
  host.SetDropHint("Finish or cancel the current one first");
  context->Update();
  ASSERT_TRUE(Hint()->IsVisible());
  EXPECT_TRUE(Hint()->IsClassSet("drop-hint--no"));
  ui::Element* refusal = ByClass("drop-hint-label--no");
  ASSERT_TRUE(Shown(refusal));
  EXPECT_EQ(std::string(refusal->GetInnerRML().c_str()), "Finish or cancel the current one first");
  EXPECT_FALSE(Shown(ByClass("drop-hint-limit"))) << "no limit line";
  EXPECT_FALSE(Shown(ByClass("drop-hint-name"))) << "no name span";
  ui::Element* badge = ByClass("drop-hint-badge");
  ASSERT_TRUE(Shown(badge));
  EXPECT_NEAR(SizeOf(badge).x, 56.f, 0.5f) << "the same round badge";
  // Grey, not the accent: the colour sheet gives the refused badge and ring their own rules.
  const std::string colours = Asset("themes/colors-light.rcss");
  EXPECT_TRUE(Has(colours, ".drop-hint--no .drop-hint-badge")) << "a neutral badge for a refusal";
  EXPECT_TRUE(Has(colours, ".drop-hint--no .drop-hint-ring")) << "and a grey ring";
  host.SetDropHint("Drop to send to Ada", NameWords{"Drop to send to ", "Ada", ""});
  context->Update();
  EXPECT_FALSE(Hint()->IsClassSet("drop-hint--no")) << "back to the accent";
}

TEST_P(DropHintRenderTest, TheRingIsInsetTenDpFromThePaneRoundedAndTakesNoClick) {
  Load();
  host.SetDropHint("Drop to send to Ada", NameWords{"Drop to send to ", "Ada", ""});
  context->Update();
  ui::Element* ring = ByClass("drop-hint-ring");
  ui::Element* pane = Pane();
  ASSERT_TRUE(ring && pane);
  const ui::Vector2f pane_at = AtOf(pane);
  const ui::Vector2f pane_size = SizeOf(pane);
  EXPECT_NEAR(AtOf(ring).x - pane_at.x, 10.f, 0.5f);
  EXPECT_NEAR(AtOf(ring).y - pane_at.y, 10.f, 0.5f);
  EXPECT_NEAR(pane_at.x + pane_size.x - (AtOf(ring).x + SizeOf(ring).x), 10.f, 0.5f);
  EXPECT_NEAR(pane_at.y + pane_size.y - (AtOf(ring).y + SizeOf(ring).y), 10.f, 0.5f);
  EXPECT_NEAR(ring->GetComputedValues().border_top_left_radius(), 16.f, 0.5f);
  EXPECT_NEAR(ring->GetComputedValues().border_top_width(), 2.f, 0.5f);
  EXPECT_EQ(ring->GetComputedValues().pointer_events(), ui::Style::PointerEvents::None);
  EXPECT_EQ(Hint()->GetComputedValues().pointer_events(), ui::Style::PointerEvents::None);
}

// ChatController::ChatPaneContains is this function on the live document: a drop counts only inside the chat's own pane.
TEST_P(DropHintRenderTest, ThePaneHitTestIncludesTheTopLeftEdgeAndExcludesTheBottomRightEdge) {
  Load();
  ui::Element* pane = Pane();
  ASSERT_NE(pane, nullptr);
  const ui::Vector2f origin = pane->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = pane->GetBox().GetSize(ui::BoxArea::Border);
  ASSERT_GT(size.x, 50.f);
  ASSERT_GT(size.y, 50.f);
  const LayoutMode mode = GetParam();
  const int left = static_cast<int>(std::ceil(origin.x));
  const int top = static_cast<int>(std::ceil(origin.y));
  const int right = static_cast<int>(std::ceil(origin.x + size.x)) - 1; // the last whole point inside
  const int bottom = static_cast<int>(std::ceil(origin.y + size.y)) - 1;
  const int mid_x = (left + right) / 2;
  const int mid_y = (top + bottom) / 2;

  EXPECT_TRUE(DropPaneContains(document, mode, {mid_x, mid_y}));
  EXPECT_TRUE(DropPaneContains(document, mode, {left, top})) << "just inside, top left";
  EXPECT_TRUE(DropPaneContains(document, mode, {right, bottom})) << "just inside, bottom right";
  EXPECT_FALSE(DropPaneContains(document, mode, {left - 1, mid_y})) << "just outside, left";
  EXPECT_FALSE(DropPaneContains(document, mode, {mid_x, top - 1})) << "just outside, above";
  EXPECT_FALSE(DropPaneContains(document, mode, {right + 1, mid_y})) << "just outside, right";
  EXPECT_FALSE(DropPaneContains(document, mode, {mid_x, bottom + 1})) << "just outside, below";
}

TEST_P(DropHintRenderTest, AnotherLayoutsPaneOrNoDocumentIsNeverHit) {
  Load();
  const LayoutMode other = GetParam() == LayoutMode::Expanded ? LayoutMode::Compact : LayoutMode::Expanded;
  EXPECT_FALSE(DropPaneContains(document, other, {500, 300})) << "that layout's pane is not in this page";
  EXPECT_FALSE(DropPaneContains(nullptr, GetParam(), {500, 300}));
}

INSTANTIATE_TEST_SUITE_P(DesktopChatLayouts, DropHintRenderTest,
                         testing::Values(LayoutMode::Expanded, LayoutMode::Compact),
                         [](const testing::TestParamInfo<LayoutMode>& info) {
                           return info.param == LayoutMode::Expanded ? "Wide" : "Narrow";
                         });

} // namespace
} // namespace pbr
