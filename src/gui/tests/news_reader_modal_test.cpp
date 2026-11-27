// The wide news reader as a modal panel (PaneSpec::modal): laid out with the real stylesheets inside the shell's real
// markup (ShellLayoutRml::ShellRoot, an expanded window with the Chats list, a chat and an open reader layer) and the
// window's title bar and status bar around it. The scrim is the window's box, the panel sits centred in it above every
// other layer, a pointer over the rail, the list, the transcript and the composer lands on the scrim / panel and never
// hovers what is behind, the panel is min(720, window - 96) wide and at most window - 96 high with the body scrolling
// inside and the bottom row pinned, the title keeps to three lines. The phone's sheet is untouched (its own test).
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellLayoutRml.h"
#include "lib/ai/MarkdownToRml.h"

#include <gtest/gtest.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>

#include <algorithm>
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
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string Asset(const char* relative) {
  return ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
}

constexpr float kTitlebar = 36.f;
constexpr float kStatusbar = 24.f;

struct Box {
  float left, top, right, bottom;
  float W() const { return right - left; }
  float H() const { return bottom - top; }
  float MidX() const { return (left + right) / 2; }
  float MidY() const { return (top + bottom) / 2; }
};

class NewsReaderModalTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    ASSERT_TRUE(ui::LoadFontFace(Path("fonts/LatoLatin-Regular.ttf")));
    ASSERT_TRUE(ui::LoadFontFace(Path("fonts/NotoSansSC-Medium.otf"), /*fallback_face=*/true));
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext("news-reader-modal-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  static std::string Path(const char* relative) { return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative; }

  static std::string Fill(std::string rml, const std::string& id, const std::string& inner) {
    const std::string open = "id=\"" + id + "\"";
    const size_t at = rml.find(open);
    if (at == std::string::npos) {
      return rml;
    }
    const size_t gt = rml.find('>', at);
    rml.insert(gt + 1, inner);
    return rml;
  }

  /** The window `width` x `height` dp: title bar, the shell's markup (rail, Chats list, chat, the reader layer when
   *  `with_reader`), the status bar. */
  void Load(const int width, const int height, const bool with_reader, const std::string& title,
            const std::string& digest) {
    state_.news_reader_title = title.c_str();
    state_.news_reader_body_rml = MarkdownToRml(digest, MarkdownOptions{.links = false}).rml.c_str();
    state_.news_reader_has_nav = true;
    state_.news_reader_can_prev = true;
    state_.news_reader_can_next = true;
    context_ = ui::CreateContext("news-reader-modal-test", {width, height});
    ASSERT_NE(context_, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context_, "chat", [this](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state_); }));

    ShellState shell;
    shell.layout_mode = LayoutMode::Expanded;
    shell.nav_tab = NavTab::Sessions;
    shell.panes.push_back({.spec = {.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true}, .id = 1});
    shell.primary_pane_key = "chat";
    if (with_reader) {
      OverlayEntry reader;
      reader.id = 7;
      reader.key = "news_reader";
      reader.rml_path = "views/news_reader_wide.rml";
      reader.page_sheet = true; // phones only: ignored on the desktop
      reader.modal = true;
      shell.overlay_stack.push_back(reader);
    }
    std::string root = ShellLayoutRml(shell, false, "").ShellRoot();
    root = std::regex_replace(root, std::regex(" data-model=\"window\""), "");
    root =
        Fill(root, "shell-nav-rail-mount", "<button id=\"hit-rail\" style=\"width: 48dp; height: 48dp;\">A</button>");
    root = Fill(root, "pane-body-sidebar",
                "<div id=\"hit-list\" class=\"sidebar-session\" style=\"height: 60dp;\">Ann</div>");
    root = Fill(root, "pane-body-chat",
                "<div class=\"message-row message-row-peer\"><div class=\"msg-hover\"><div><div class=\"bubble "
                "bubble-peer\" id=\"hit-bubble\" style=\"width: 200dp; height: 40dp;\"><p>Hi</p></div></div>"
                "<div class=\"msg-bar\" id=\"hit-bar\"><button>+</button></div></div></div>");
    root = Fill(root, "pane-composer-chat",
                "<textarea id=\"draft-input\" style=\"width: 300dp; height: 40dp;\"></textarea>");
    if (with_reader) {
      root = Fill(root, "overlay-body-7",
                  LocalizationService::Instance().LocalizeText(Asset("views/news_reader_wide.rml")));
    }
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body id=\"body\" class=\"shell-body shell-body--wide" +
        std::string(with_reader ? " shell-body--modal" : "") +
        "\" style=\"padding: 0; font-size: 15dp;\">"
        "<div id=\"shell-titlebar\" class=\"shell-titlebar\"><button id=\"hit-titlebtn\" "
        "class=\"shell-titlebar-btn\"></button></div>"
        "<div id=\"shell-root\" class=\"shell-root\" style=\"top: " +
        std::to_string(static_cast<int>(kTitlebar)) + "dp; bottom: " + std::to_string(static_cast<int>(kStatusbar)) +
        "dp;\">" + root +
        "</div><div id=\"shell-statusbar\" class=\"shell-statusbar\"><div id=\"hit-status\" "
        "class=\"shell-statusbar-cluster\" style=\"width: 60dp; height: 20dp;\"></div></div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    if (with_reader) {
      // What ShellHost::ApplySafeAreaLayout does for a modal layer: reach out of #shell-root over the title bar's strip
      // and the status bar.
      for (const char* part : {"shell-modal-scrim-7", "shell-modal-stage-7"}) {
        ui::Element* element = Id(part);
        ASSERT_NE(element, nullptr) << part;
        element->SetProperty("top", "-36dp");
        element->SetProperty("bottom", "-24dp");
      }
    }
    for (int i = 0; i < 3; ++i) {
      context_->Update();
    }
  }

  static Box BoxOf(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  ui::Element* First(const char* cls) {
    ui::ElementList found;
    document_->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found[0];
  }
  ui::Element* Id(const char* id) { return document_->GetElementById(id); }
  static bool Within(ui::Element* hit, ui::Element* ancestor) {
    for (; hit; hit = hit->GetParentNode()) {
      if (hit == ancestor) {
        return true;
      }
    }
    return false;
  }
  /** What a pointer at the centre of `behind` reaches. */
  ui::Element* HitAtCentreOf(ui::Element* behind) {
    const Box b = BoxOf(behind);
    return context_->GetElementAtPoint({b.MidX(), b.MidY()});
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ChatScreenState state_;
};

std::string LongDigest() {
  std::string text = "The Hindu · November 23, 2026\n\n";
  for (int i = 0; i < 40; ++i) {
    text +=
        "The parties concluded a new round of talks without reaching an agreement on a coalition, which would "
        "require concessions from both sides.\n\n";
  }
  return text;
}

const char* kShortTitle = "A Russian cargo ship remains on fire off Finland";

TEST_F(NewsReaderModalTest, TheScrimIsTheWholeWindowAndThePanelIsCentredInItAboveEveryLayer) {
  for (const int width : {768, 1000, 1440}) {
    SCOPED_TRACE(width);
    constexpr int kHeight = 700;
    Load(width, kHeight, true, kShortTitle, LongDigest());
    ui::Element* scrim = First("shell-scrim");
    ui::Element* frame = First("shell-frame--news_reader");
    ASSERT_NE(scrim, nullptr);
    ASSERT_NE(frame, nullptr);
    const Box s = BoxOf(scrim);
    EXPECT_NEAR(s.left, 0.f, 0.5f);
    EXPECT_NEAR(s.top, 0.f, 0.5f);
    EXPECT_NEAR(s.W(), static_cast<float>(width), 0.5f) << "the scrim covers the window's width, rail included";
    EXPECT_NEAR(s.H(), static_cast<float>(kHeight), 0.5f)
        << "and its height: the title bar and the status bar are under it too";
    const Box f = BoxOf(frame);
    EXPECT_NEAR(f.W(), std::min(720.f, static_cast<float>(width) - 96.f), 1.f) << "min(720, window - 96)";
    EXPECT_NEAR(f.MidX(), static_cast<float>(width) / 2, 1.f) << "centred horizontally in the window";
    EXPECT_GE(f.top, 48.f - 0.5f);
    EXPECT_LE(f.bottom, static_cast<float>(kHeight) - 48.f + 0.5f) << "at most window - 96 high";
    EXPECT_NEAR(f.MidY(), static_cast<float>(kHeight) / 2, 1.f) << "centred vertically in the window";
    // Above every other layer: what is at the panel's centre is inside the panel.
    EXPECT_TRUE(Within(HitAtCentreOf(frame), frame));
    document_->Close();
    context_->Update();
    DataModelHost::Instance().Clear();
    ui::RemoveContext("news-reader-modal-test");
    context_ = nullptr;
  }
}

TEST_F(NewsReaderModalTest, APointerOverTheRailTheListTheTranscriptTheComposerAndTheStatusBarReachesTheScrim) {
  Load(1000, 700, true, kShortTitle, "Short digest.");
  ui::Element* scrim = First("shell-scrim");
  ui::Element* frame = First("shell-frame--news_reader");
  ASSERT_NE(scrim, nullptr);
  ASSERT_NE(frame, nullptr);
  for (const char* id : {"hit-rail", "hit-list", "hit-bubble", "draft-input", "hit-status"}) {
    SCOPED_TRACE(id);
    ui::Element* behind = Id(id);
    ASSERT_NE(behind, nullptr);
    const Box b = BoxOf(behind);
    ASSERT_GT(b.W(), 1.f);
    ui::Element* hit = HitAtCentreOf(behind);
    ASSERT_NE(hit, nullptr);
    EXPECT_FALSE(Within(hit, behind)) << "a click would reach the element behind the panel";
    EXPECT_TRUE(Within(hit, scrim) || Within(hit, frame));
  }
  // The window's own point (0,0) and the bottom-right corner: under the scrim, not the title bar's or the status bar's
  // controls (the title bar keeps its window buttons above, they are the OS's).
  for (const ui::Vector2f at : {ui::Vector2f{3.f, 400.f}, ui::Vector2f{990.f, 695.f}, ui::Vector2f{500.f, 690.f}}) {
    ui::Element* hit = context_->GetElementAtPoint(at);
    ASSERT_NE(hit, nullptr);
    EXPECT_TRUE(Within(hit, scrim)) << at.x << "," << at.y;
  }
}

TEST_F(NewsReaderModalTest, HoveringAMessageBehindThePanelShowsNoHoverBar) {
  // Control: with no reader the pointer over the bubble shows the bar.
  Load(1000, 700, false, kShortTitle, "x");
  {
    ui::Element* bubble = Id("hit-bubble");
    const Box b = BoxOf(bubble);
    context_->ProcessMouseMove(static_cast<int>(b.MidX()), static_cast<int>(b.MidY()), 0);
    context_->Update();
    EXPECT_TRUE(Id("hit-bar")->GetComputedValues().display() != ui::Style::Display::None)
        << "without the reader the hover bar shows under the pointer (the control)";
  }
  document_->Close();
  context_->Update();
  DataModelHost::Instance().Clear();
  ui::RemoveContext("news-reader-modal-test");
  context_ = nullptr;

  Load(1000, 700, true, kShortTitle, "x");
  ui::Element* bubble = Id("hit-bubble");
  const Box b = BoxOf(bubble);
  context_->ProcessMouseMove(static_cast<int>(b.MidX()), static_cast<int>(b.MidY()), 0);
  context_->Update();
  EXPECT_EQ(Id("hit-bar")->GetComputedValues().display(), ui::Style::Display::None)
      << "the pointer is over the scrim: the bar behind stays hidden";
  EXPECT_FALSE(bubble->IsPseudoClassSet("hover"));
}

TEST_F(NewsReaderModalTest, ALongDigestScrollsInsideAPanelNoTallerThanTheWindowLessTheMarginsAndTheBarIsPinned) {
  constexpr int kHeight = 600;
  Load(900, kHeight, true,
       "Netanyahu, Eisenkot parties end talks without deal on forming Israel government after a long night of talks",
       LongDigest());
  ui::Element* frame = First("shell-frame--news_reader");
  ui::Element* scroll = First("news-reader-scroll");
  ui::Element* bar = First("news-reader-bar");
  ui::Element* title = First("news-reader-title");
  ASSERT_NE(frame, nullptr);
  ASSERT_NE(scroll, nullptr);
  ASSERT_NE(bar, nullptr);
  ASSERT_NE(title, nullptr);
  const Box f = BoxOf(frame);
  const Box s = BoxOf(scroll);
  const Box r = BoxOf(bar);
  EXPECT_NEAR(f.top, 48.f, 1.f);
  EXPECT_NEAR(f.bottom, static_cast<float>(kHeight) - 48.f, 1.f)
      << "a long digest fills the window less 48dp each side";
  EXPECT_LE(s.bottom, r.top + 0.5f) << "the scrolling body ends where the bottom row begins";
  EXPECT_NEAR(r.bottom, f.bottom, 1.5f) << "the row is pinned to the panel's bottom";
  EXPECT_GT(scroll->GetScrollHeight(), scroll->GetClientHeight() + 50.f) << "the digest scrolls inside the panel";
  EXPECT_LE(BoxOf(title).H(), 78.f + 0.5f) << "the title keeps to three lines";
  EXPECT_GT(BoxOf(title).H(), 26.f) << "and this long one wraps";
  // The three buttons are 44dp and inside the panel.
  for (const char* cls : {"news-reader-open", "news-reader-step"}) {
    ui::ElementList found;
    document_->GetElementsByClassName(found, cls);
    ASSERT_FALSE(found.empty());
    for (ui::Element* button : found) {
      EXPECT_GE(BoxOf(button).H(), 44.f);
      EXPECT_GE(BoxOf(button).left, f.left);
      EXPECT_LE(BoxOf(button).right, f.right);
    }
  }
}

TEST_F(NewsReaderModalTest, AShortDigestKeepsTheSamePanelWithTheRowStillAtTheBottom) {
  Load(1000, 700, true, kShortTitle, "Short digest.");
  const Box f = BoxOf(First("shell-frame--news_reader"));
  EXPECT_NEAR(f.W(), 720.f, 1.f);
  EXPECT_NEAR(f.MidY(), 350.f, 1.f);
  EXPECT_NEAR(f.H(), 700.f - 96.f, 1.f) << "the panel is window - 96 high, whatever the digest";
  EXPECT_NEAR(BoxOf(First("news-reader-bar")).bottom, f.bottom, 1.5f) << "the row stays at the bottom";
}

TEST_F(NewsReaderModalTest, TheCloseButtonSitsWhereTheOtherWideFramesHaveIt) {
  Load(1000, 700, true, kShortTitle, "Short digest.");
  ui::Element* close = First("shell-close-btn");
  ASSERT_NE(close, nullptr);
  const Box c = BoxOf(close);
  const Box f = BoxOf(First("shell-frame--news_reader"));
  EXPECT_NEAR(c.W(), 40.f, 0.5f);
  EXPECT_NEAR(c.H(), 40.f, 0.5f);
  EXPECT_NEAR(f.right - c.right, 8.f, 0.5f);
  EXPECT_NEAR(c.top - f.top, 8.f, 0.5f);
  const Box t = BoxOf(First("news-reader-title"));
  EXPECT_LE(t.right, c.left + 0.5f) << "the title on the left keeps clear of the close button";
}

// The phone keeps its page sheet: a modal-flagged layer in a compact window is the sheet, with no stage.
TEST(NewsReaderModalMarkupTest, ACompactWindowStillGetsThePageSheetAndAWideOneTheModalStage) {
  for (const LayoutMode mode : {LayoutMode::Compact, LayoutMode::Expanded}) {
    ShellState shell;
    shell.layout_mode = mode;
    OverlayEntry reader;
    reader.id = 3;
    reader.key = "news_reader";
    reader.page_sheet = true;
    reader.modal = true;
    shell.overlay_stack.push_back(reader);
    const std::string rml = ShellLayoutRml(shell, false, "").ShellRoot();
    const bool wide = mode == LayoutMode::Expanded;
    EXPECT_EQ(rml.find("shell-modal-stage") != std::string::npos, wide);
    EXPECT_EQ(rml.find("shell-layer-modal") != std::string::npos, wide);
    EXPECT_EQ(rml.find("shell-page-sheet--news_reader") != std::string::npos, !wide);
  }
}

} // namespace
} // namespace pbr
