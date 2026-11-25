// The wide layout's frost (owner 2026-11-23): with Settings -> Appearance -> Transparency on (reduce_transparency
// false, the default) the conversation column's header strip and composer strip carry the phone bars' material (the
// x-bar-blur colour over a 20px backdrop blur); off, the same bar colour, solid, and no blur anywhere. The shell root
// is ShellLayoutRml's, the pages the real views, the stylesheets the real ones. The cost limit: at most two blurred
// regions on a wide page (never more than three), never a whole column.
#include "gui/chat/ChatController.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellLayoutRml.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ui/ShellInterruption.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <ui/style/ComputedValues.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string Asset(const std::string& relative) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

/** Puts `body` into the empty element `id` of `root` (a pane body, the composer mount, a settings section mount). */
void Fill(std::string& root, const std::string& id, const std::string& body) {
  const std::string empty = "id=\"" + id + "\"></div>";
  const size_t at = root.find(empty);
  if (at != std::string::npos) {
    root.replace(at, empty.size(), "id=\"" + id + "\">" + body + "</div>");
  }
}

ShellState Wide(const NavTab tab, const char* primary) {
  ShellState state;
  state.layout_mode = LayoutMode::Expanded;
  state.nav_tab = tab;
  if (primary[0] != '\0') {
    const bool chat = std::string(primary) == "chat";
    state.panes.push_back({.spec = {.key = primary, .rml_path = "views/chat.rml", .provides_composer = chat}, .id = 1});
    state.primary_pane_key = primary;
  }
  return state;
}

size_t Count(const std::string& text, const std::string& part) {
  size_t n = 0;
  for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) {
    ++n;
  }
  return n;
}

class WideFrostLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/LatoLatin-Regular.ttf"));
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/Inter-Regular.ttf", true));
    context_ = ui::CreateContext(kContext, {1280, 800});
    ASSERT_NE(context_, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context_, "chat", [this](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, chat_); }));
    chat_.use_messages_layout = true;
    chat_.has_turns = true;
    chat_.thread_title = "Ann Lee";
    chat_.show_search = true;
    chat_.show_thread_menu = true;
    for (int i = 0; i < 30; ++i) {
      MessageDisplayRow row;
      row.message_id = "m" + std::to_string(i);
      row.row_class = i % 2 == 0 ? "message-row-user" : "message-row-peer";
      row.content_rml = "<div class=\"bubble bubble-user\"><p class=\"bubble-text\">Hello there</p></div>";
      chat_.messages.push_back(row);
    }
  }
  void TearDown() override {
    if (document_) {
      document_->Close();
    }
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext(kContext);
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  /** The shell root of `state` in a 1280 x 800 window (wide) or 390 x 800 (phone), its panes filled with their views. */
  void Load(const ShellState& state, const char* theme = "light") {
    if (document_) {
      document_->Close();
      document_ = nullptr;
      context_->Update();
    }
    const bool wide = state.layout_mode == LayoutMode::Expanded;
    std::string root = ShellLayoutRml(state, false, "").ShellRoot();
    Fill(root, "pane-body-chat", Asset(wide ? "views/chat_wide.rml" : "views/chat.rml"));
    Fill(root, "pane-composer-chat", Asset("views/composer_wide.rml"));
    Fill(root, "shell-composer-mount", Asset("views/composer.rml"));
    Fill(root, "pane-body-sidebar", Asset("views/sidebar.rml"));
    Fill(root, "pane-body-ai_list", Asset("views/ai_list_wide.rml"));
    Fill(root, "pane-body-home", Asset("views/home.rml"));
    Fill(root, "pane-body-settings", Asset("views/settings_wide.rml"));
    Fill(root, "pane-body-settings_detail", Asset("views/settings_detail.rml"));
    Fill(root, "settings-section-mount-pane", Asset("views/settings_section_appearance.rml"));
    const std::string width = wide ? std::to_string(window_w_) + "dp" : "390dp";
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/wide.rcss") + Asset("themes/colors-light.rcss") + Asset("themes/colors-dark.rcss") +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div class=\"shell-body " +
        std::string(wide ? "shell-body--wide" : "shell-body--compact") +
        "\" style=\"position: absolute; top: 0; left: 0; width: " + width + "; height: " + std::to_string(window_h_) +
        "dp;\">" + LocalizationService::Instance().LocalizeText(root) + "</div></body></rml>";
    context_->ActivateTheme("light", std::string(theme) == "light");
    context_->ActivateTheme("dark", std::string(theme) == "dark");
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    context_->Update();
  }

  /** Every drawn element whose computed style carries a backdrop blur (display:none subtrees are not drawn). */
  std::vector<ui::Element*> Blurred() {
    std::vector<ui::Element*> found;
    Walk(document_, found);
    return found;
  }
  static void Walk(ui::Element* element, std::vector<ui::Element*>& found) {
    if (element->GetComputedValues().display() == ui::Style::Display::None) {
      return;
    }
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    if (element->GetComputedValues().has_backdrop_filter() && size.x > 0.f && size.y > 0.f) {
      found.push_back(element);
    }
    for (int i = 0; i < element->GetNumChildren(); ++i) {
      Walk(element->GetChild(i), found);
    }
  }
  ui::Element* First(const char* selector) { return document_->QuerySelector(selector); }
  float Top(ui::Element* e) { return e->GetAbsoluteOffset(ui::BoxArea::Border).y; }
  float Bottom(ui::Element* e) { return Top(e) + e->GetBox().GetSize(ui::BoxArea::Border).y; }
  float Height(ui::Element* e) { return e->GetBox().GetSize(ui::BoxArea::Border).y; }

  int window_w_ = 1280;
  int window_h_ = 800;
  static constexpr const char* kContext = "wide-frost-test";
  NullRender render_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ChatScreenState chat_;
};

// The decision lives in ResolveFrost: the wide layout frosts its header and composer strips while nothing covers the
// base; the phone's answers are those of shell_host_test.
TEST(WideFrostResolveTest, TheWideBaseFrostsItsHeaderAndComposerStripsAndNothingUnderAnInterruption) {
  ShellState wide = Wide(NavTab::Sessions, "chat");
  EXPECT_EQ(ShellInterruption::ResolveFrost(wide).top, CompactChromeFrostSurface::WidePaneHeader);
  EXPECT_EQ(ShellInterruption::ResolveFrost(wide).bottom, CompactChromeFrostSurface::WideComposer);
  ShellState dialog = wide;
  dialog.dialog.active = true;
  EXPECT_EQ(ShellInterruption::ResolveFrost(dialog).top, CompactChromeFrostSurface::None);
  EXPECT_EQ(ShellInterruption::ResolveFrost(dialog).bottom, CompactChromeFrostSurface::None);
  ShellState pushed = wide;
  PaneState page;
  page.spec.key = "contact_detail";
  pushed.transient_stack.push_back(page);
  EXPECT_EQ(ShellInterruption::ResolveFrost(pushed).top, CompactChromeFrostSurface::None);
}

// The shell markup: the frost class on the row and the composer strip's material follow Transparency, nothing else.
TEST(WideFrostResolveTest, TheWideRowAndComposerStripFollowTheTransparencySwitch) {
  ShellState state = Wide(NavTab::Sessions, "chat");
  const std::string on = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_EQ(Count(on, "shell-pane-row--frost"), 1U);
  EXPECT_EQ(Count(on, "bar-material bar-material--frost"), 1U) << "the composer strip's material";
  const size_t bar = on.find("class=\"shell-pane-composer-bar\"");
  ASSERT_NE(bar, std::string::npos);
  EXPECT_LT(on.find("bar-material", bar), on.find("id=\"pane-composer-chat\"", bar)) << "the material comes first";

  state.reduce_transparency = true;
  const std::string off = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_EQ(Count(off, "shell-pane-row--frost"), 0U);
  EXPECT_EQ(Count(off, "bar-material--frost"), 0U);
  EXPECT_EQ(Count(off, "class=\"bar-material\""), 1U) << "the composer strip keeps its material, solid";

  state.reduce_transparency = false;
  state.compact_chrome_frost = false;
  EXPECT_EQ(Count(ShellLayoutRml(state, false, "").ShellRoot(), "bar-material--frost"), 0U);

  state.compact_chrome_frost = true;
  state.dialog.active = true;
  const std::string under = ShellLayoutRml(state, false, "").ShellRoot();
  EXPECT_EQ(Count(under, "shell-pane-row--frost"), 0U) << "a dialog over the base leaves the strips solid";
  EXPECT_EQ(Count(under, "bar-material--frost"), 0U);
}

TEST_F(WideFrostLayoutTest, TheChatPageFrostsTheHeaderAndComposerStripsAndTheTranscriptRunsUnderBoth) {
  Load(Wide(NavTab::Sessions, "chat"));
  const std::vector<ui::Element*> blurred = Blurred();
  ASSERT_EQ(blurred.size(), 2U) << "two blurred regions: the header strip and the composer strip";
  ui::Element* header = First(".chat-header");
  ui::Element* bar = First(".shell-pane-composer-bar");
  ASSERT_TRUE(header && bar);
  EXPECT_EQ(blurred[0]->GetParentNode(), header);
  EXPECT_EQ(blurred[1]->GetParentNode(), bar);
  ui::Element* column = First("#pane-chat");
  ui::Element* messages = First("#chat-messages");
  ASSERT_TRUE(column && messages);
  for (ui::Element* strip : blurred) {
    EXPECT_LT(Height(strip), 0.25f * Height(column)) << "a strip, never a column";
  }
  EXPECT_NEAR(Height(blurred[0]), Height(header), 1.5f) << "the whole strip above its hairline";
  EXPECT_LE(Top(messages), Top(header) + 0.5f) << "the transcript scrolls up under the header strip";
  EXPECT_NEAR(Bottom(bar), Bottom(column), 0.5f) << "the composer strip sits on the column's bottom edge";
  EXPECT_GE(Bottom(messages), Bottom(column) - 0.5f) << "and the transcript runs down under it";
  EXPECT_GT(Height(bar), 40.f) << "the composer is mounted in the strip";
  EXPECT_NEAR(Height(blurred[1]), Height(bar) - 12.f, 0.5f) << "the panel is the strip minus its 12dp bottom margin";

  // The header strip is the phone bar's see-through colour (x-bar-blur, 78%); the floating composer panel is more
  // translucent still (x-composer-float, 62%) so the messages read through it.
  EXPECT_LT(blurred[0]->GetComputedValues().background_color().alpha, 255);
  EXPECT_LT(blurred[1]->GetComputedValues().background_color().alpha,
            blurred[0]->GetComputedValues().background_color().alpha);
  EXPECT_GT(blurred[1]->GetComputedValues().background_color().alpha, 0);
}

TEST_F(WideFrostLayoutTest, TransparencyOffLeavesTheSameBarColourSolidAndNoBlurOnTheChatPage) {
  ShellState state = Wide(NavTab::Sessions, "chat");
  state.reduce_transparency = true;
  Load(state);
  EXPECT_TRUE(Blurred().empty());
  ui::Element* header_material = First(".chat-header > .bar-material");
  ui::Element* composer_material = First(".shell-pane-composer-bar > .bar-material");
  ASSERT_TRUE(header_material && composer_material);
  EXPECT_EQ(header_material->GetComputedValues().background_color().alpha, 255) << "solid";
  EXPECT_EQ(header_material->GetComputedValues().background_color(),
            composer_material->GetComputedValues().background_color())
      << "the floating panel is the bar colour, solid";
}

/** The composer panel of the chat page: its material (the shape), the strip behind it, the mount. */
struct ComposerParts {
  ui::Element* bar = nullptr;
  ui::Element* material = nullptr;
  ui::Element* mount = nullptr;
  ui::Element* column = nullptr;
};

ComposerParts PartsOf(ui::ElementDocument* document) {
  ComposerParts parts;
  parts.bar = document->QuerySelector(".shell-pane-composer-bar");
  parts.material = document->QuerySelector(".shell-pane-composer-bar > .bar-material");
  parts.mount = document->QuerySelector("#pane-composer-chat");
  parts.column = document->QuerySelector("#pane-chat");
  return parts;
}

// The composer floats: one rounded panel 16dp in from the column's left, right and bottom edges; the strip behind it is
// clear (no band); the panel has a hairline and the blur-free stacked shadow, and is blurred and see-through with
// Transparency on, solid and unblurred with it off, in both themes. The mount is the panel plus its bottom margin, so the
// transcript's end spacer (KeepClearOfComposer measures the mount) leaves the newest message above the panel.
TEST_F(WideFrostLayoutTest, TheComposerIsOneFloatingRoundedPanelInTheColumnsMargins) {
  for (const bool reduce : {false, true}) {
    for (const char* theme : {"light", "dark"}) {
      SCOPED_TRACE(std::string(theme) + (reduce ? " solid" : " frosted"));
      ShellState state = Wide(NavTab::Sessions, "chat");
      state.reduce_transparency = reduce;
      Load(state, theme);
      const ComposerParts p = PartsOf(document_);
      ASSERT_TRUE(p.bar && p.material && p.mount && p.column);
      const float dp = context_->GetDensityIndependentPixelRatio();
      const auto left = [](ui::Element* e) { return e->GetAbsoluteOffset(ui::BoxArea::Border).x; };
      const auto width = [](ui::Element* e) { return e->GetBox().GetSize(ui::BoxArea::Border).x; };
      EXPECT_NEAR(left(p.material), left(p.column) + 12.f, 0.5f);
      EXPECT_NEAR(left(p.material) + width(p.material), left(p.column) + width(p.column) - 12.f, 0.5f);
      EXPECT_NEAR(Bottom(p.material), Bottom(p.column) - 12.f, 0.5f);
      EXPECT_NEAR(Bottom(p.mount), Bottom(p.column), 0.5f);
      EXPECT_NEAR(Height(p.mount), Height(p.material) + 12.f, 0.5f);
      EXPECT_GE(p.material->GetComputedValues().border_top_left_radius(), 20.f * dp - 0.5f);
      EXPECT_GT(p.material->GetComputedValues().border_top_width(), 0.f) << "a hairline";
      const ui::String shadow = p.material->GetProperty<ui::String>("box-shadow");
      EXPECT_FALSE(shadow.empty() || shadow == "none") << "the stacked shadow lifts it off the messages";
      EXPECT_EQ(p.bar->GetComputedValues().background_color().alpha, 0) << "no band behind the panel";
      // The row, the buttons and the hint are inside the panel.
      for (const char* inner : {".prompt-composer-row", ".prompt-composer-pill"}) {
        ui::Element* e = document_->QuerySelector(inner);
        ASSERT_NE(e, nullptr) << inner;
        EXPECT_GE(left(e), left(p.material)) << inner;
        EXPECT_LE(left(e) + width(e), left(p.material) + width(p.material) + 0.5f) << inner;
        EXPECT_GE(Top(e), Top(p.material) - 0.5f) << inner;
        EXPECT_LE(Bottom(e), Bottom(p.material) + 0.5f) << inner;
      }
      const ui::Colourb fill = p.material->GetComputedValues().background_color();
      if (reduce) {
        EXPECT_TRUE(Blurred().empty());
        EXPECT_EQ(fill.alpha, 255);
      } else {
        const std::vector<ui::Element*> blurred = Blurred();
        ASSERT_EQ(blurred.size(), 2U) << "header + composer";
        EXPECT_EQ(blurred[1], p.material);
        EXPECT_GT(fill.alpha, 0);
        EXPECT_LT(fill.alpha, 199) << "more translucent than the phone bars' 78%";
      }
    }
  }
}

// The engine draws no placeholder, so the "Enter to send" hint is a label over the empty field: shown while the draft is
// empty, gone with any text and while recording, inside the input pill and clear of the camera key, one ellipsized line
// at the narrowest window; the phone's markup does not get it.
TEST_F(WideFrostLayoutTest, TheWideComposerShowsItsHintOverTheEmptyFieldOnly) {
  const std::string hint = LocalizationService::Instance().LocalizeText("{{i18n:chat.composer.hint_wide}}");
  const auto drawn = [&](ui::Element* e) {
    return e && e->GetComputedValues().display() != ui::Style::Display::None && Height(e) > 0.f;
  };
  for (const int width : {900, 1280}) {
    SCOPED_TRACE(width);
    window_w_ = width;
    chat_.show_capture = true;
    Load(Wide(NavTab::Sessions, "chat"));
    ui::Element* label = First(".prompt-composer-label--wide");
    ui::Element* pill = First(".prompt-composer-pill");
    ui::Element* input = First("#draft-input");
    ui::Element* capture = First(".prompt-capture-btn");
    ASSERT_TRUE(label && pill && input && capture);
    EXPECT_TRUE(drawn(label));
    EXPECT_EQ(label->GetInnerRML(), hint);
    const auto left = [](ui::Element* e) { return e->GetAbsoluteOffset(ui::BoxArea::Border).x; };
    const auto right = [&](ui::Element* e) { return left(e) + e->GetBox().GetSize(ui::BoxArea::Border).x; };
    EXPECT_GE(left(label), left(pill));
    EXPECT_LE(right(label), right(pill) + 0.5f);
    EXPECT_GE(Top(label), Top(pill) - 0.5f);
    EXPECT_LE(Bottom(label), Bottom(pill) + 0.5f);
    ASSERT_TRUE(drawn(capture));
    EXPECT_LE(right(label), left(capture) + 0.5f) << "clear of the camera key";
    EXPECT_NEAR((Top(label) + Bottom(label)) / 2.f, (Top(input) + Bottom(input)) / 2.f, 1.5f) << "on the input's line";
    EXPECT_LT(Height(label), 44.f) << "one line, not wrapped";
    EXPECT_EQ(label->GetComputedValues().pointer_events(), ui::Style::PointerEvents::None);

    chat_.draft = "hi";
    Load(Wide(NavTab::Sessions, "chat"));
    EXPECT_FALSE(drawn(First(".prompt-composer-label--wide"))) << "any text hides it";

    chat_.draft = "";
    chat_.voice_recording = true;
    chat_.voice_timer = "0:03";
    Load(Wide(NavTab::Sessions, "chat"));
    EXPECT_FALSE(drawn(First(".prompt-composer-label--wide"))) << "recording hides it";
    chat_.voice_recording = false;
  }
  std::string phone = Asset("views/composer.rml");
  EXPECT_EQ(phone.find("prompt-composer-label--wide"), std::string::npos);
  EXPECT_EQ(phone.find("hint_wide"), std::string::npos);
}

// Every state of the composer stays inside the panel (the mount is the panel's padding): the reply / edit strip, the
// status row (an attachment chip, the stop key), and the recording pill; the panel grows upward and the mount follows.
TEST_F(WideFrostLayoutTest, TheComposersStripsAndRecordingStayInsideTheFloatingPanel) {
  struct Case {
    const char* name;
    const char* inner;
    void (*apply)(ChatScreenState&);
  };
  const Case cases[] = {
      {"reply strip", ".prompt-quote",
       [](ChatScreenState& s) {
         s.quote_reply = true;
         s.quote_reply_text = "Earlier message";
       }},
      {"edit strip", ".prompt-quote",
       [](ChatScreenState& s) {
         s.editing_message = true;
         s.quote_reply_text = "Edit message";
       }},
      {"picture chip", ".prompt-composer-status", [](ChatScreenState& s) { s.image_chip = true; }},
      {"stop key", ".prompt-composer-status", [](ChatScreenState& s) { s.loading = true; }},
      {"recording", ".prompt-voice-live-pill",
       [](ChatScreenState& s) {
         s.voice_recording = true;
         s.voice_timer = "0:03";
       }},
  };
  float plain_height = 0.f;
  {
    Load(Wide(NavTab::Sessions, "chat"));
    plain_height = Height(PartsOf(document_).material);
  }
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    ChatScreenState fresh;
    fresh.use_messages_layout = chat_.use_messages_layout;
    fresh.has_turns = true;
    fresh.thread_title = chat_.thread_title;
    fresh.messages = chat_.messages;
    c.apply(fresh);
    chat_ = fresh;
    Load(Wide(NavTab::Sessions, "chat"));
    const ComposerParts p = PartsOf(document_);
    ASSERT_TRUE(p.material && p.mount);
    ui::Element* inner = document_->QuerySelector(c.inner);
    ASSERT_NE(inner, nullptr);
    ASSERT_GT(Height(inner), 0.f);
    const float inner_left = inner->GetAbsoluteOffset(ui::BoxArea::Border).x;
    const float inner_right = inner_left + inner->GetBox().GetSize(ui::BoxArea::Border).x;
    const float m_left = p.material->GetAbsoluteOffset(ui::BoxArea::Border).x;
    const float m_right = m_left + p.material->GetBox().GetSize(ui::BoxArea::Border).x;
    EXPECT_GE(inner_left, m_left);
    EXPECT_LE(inner_right, m_right + 0.5f);
    EXPECT_GE(Top(inner), Top(p.material) - 0.5f) << "inside, not above the panel";
    EXPECT_LE(Bottom(inner), Bottom(p.material) + 0.5f);
    EXPECT_NEAR(Height(p.mount), Height(p.material) + 12.f, 0.5f);
    if (std::string(c.inner) != ".prompt-voice-live-pill") {
      EXPECT_GT(Height(p.material), plain_height) << "the panel grows to hold it";
    }
  }
}

// The cost limit on every wide page, Transparency on: at most two blurred regions (never more than three).
TEST_F(WideFrostLayoutTest, EveryWidePageKeepsToTwoBlurredRegionsAtMost) {
  struct Page {
    const char* name;
    ShellState state;
    size_t expected;
  };
  ShellState home = Wide(NavTab::Home, "");
  home.panes.push_back({.spec = {.key = "home", .rml_path = "views/home.rml"}, .id = 2});
  const std::vector<Page> pages = {
      {"chat", Wide(NavTab::Sessions, "chat"), 2U},
      {"chats list, nothing picked", Wide(NavTab::Sessions, ""), 0U},
      {"settings, Appearance", Wide(NavTab::Me, "settings_detail"), 0U},
      {"AI", home, 0U},
      {"AI conversation", Wide(NavTab::Home, "chat"), 2U},
  };
  for (const Page& page : pages) {
    for (const char* theme : {"light", "dark"}) {
      Load(page.state, theme);
      const size_t n = Blurred().size();
      EXPECT_EQ(n, page.expected) << page.name << " (" << theme << ")";
      EXPECT_LE(n, 3U) << page.name;
    }
  }
}

// Across the breakpoint the chat header keeps its material: the phone chat bar's and the wide header strip's are the
// same colour, blurred alike with Transparency on, solid alike with it off.
TEST_F(WideFrostLayoutTest, ThePhoneAndWideChatHeadersHaveTheSameMaterialOnBothSidesOfTheBreakpoint) {
  for (const bool reduce : {false, true}) {
    for (const char* theme : {"light", "dark"}) {
      ShellState phone = Wide(NavTab::Sessions, "chat");
      phone.layout_mode = LayoutMode::Compact;
      phone.compact_chat_open = true;
      phone.reduce_transparency = reduce;
      Load(phone, theme);
      ui::Element* phone_material = First(".shell-chat-overlay-chrome > .bar-material");
      ASSERT_NE(phone_material, nullptr);
      const ui::Colourb phone_colour = phone_material->GetComputedValues().background_color();
      const bool phone_blur = phone_material->GetComputedValues().has_backdrop_filter();

      ShellState wide = Wide(NavTab::Sessions, "chat");
      wide.reduce_transparency = reduce;
      Load(wide, theme);
      ui::Element* wide_material = First(".chat-header > .bar-material");
      ASSERT_NE(wide_material, nullptr);
      EXPECT_EQ(wide_material->GetComputedValues().background_color(), phone_colour) << theme << " reduce=" << reduce;
      EXPECT_EQ(wide_material->GetComputedValues().has_backdrop_filter(), phone_blur) << theme << " reduce=" << reduce;
      EXPECT_EQ(phone_blur, !reduce);
    }
  }
}

// The switch is the phone's: Appearance shows Theme, Language, Transparency in that order on both layouts, the
// Transparency switch drawn on the wide page too.
TEST_F(WideFrostLayoutTest, TheTransparencyRowIsOnTheWideAppearancePageInThePhonesOrder) {
  const std::string section = Asset("views/settings_section_appearance.rml");
  const size_t theme = section.find("on_choose_theme()");
  const size_t language = section.find("on_choose_language()");
  const size_t transparency = section.find("toggle_transparency()");
  ASSERT_NE(transparency, std::string::npos);
  EXPECT_LT(theme, language);
  EXPECT_LT(language, transparency);
  EXPECT_EQ(Count(section, "toggle_transparency()"), 1U);

  Load(Wide(NavTab::Me, "settings_detail"));
  ui::ElementList switches;
  document_->QuerySelectorAll(switches, "#settings-section-mount-pane .settings-switch");
  ui::Element* row = nullptr;
  for (ui::Element* candidate : switches) {
    if (candidate->GetAttribute<ui::String>("data-event-click", "") == "toggle_transparency()") {
      row = candidate;
    }
  }
  ASSERT_NE(row, nullptr) << "the wide Appearance page has the Transparency switch";
  EXPECT_GT(row->GetBox().GetSize(ui::BoxArea::Border).y, 0.f) << "and draws it";
  for (ui::Element* e = row; e; e = e->GetParentNode()) {
    EXPECT_NE(e->GetComputedValues().display(), ui::Style::Display::None);
  }
}

/** One frame of the real loop for the transcript: the spacer follows the panel (KeepClearOfComposer), the layout runs,
 *  and a transcript that was at the bottom stays there (what ChatTranscriptScroller does for a pinned one). */
struct Frames {
  static void Run(ui::Context* context, ui::ElementDocument* document, const bool pinned) {
    for (int i = 0; i < 4; ++i) {
      ChatController::KeepClearOfComposer(document->QuerySelector("#pane-chat"), "pane-composer-chat");
      context->Update();
      if (pinned) {
        ui::Element* messages = document->QuerySelector("#chat-messages");
        messages->SetScrollTop(messages->GetScrollHeight());
        context->Update();
      }
    }
  }
};

// The newest message rests above the floating panel when the conversation is at its bottom: its bottom edge is at least
// 8dp above the panel's top edge, in a chat, a group (avatar column) and an AI conversation, at both pixel ratios, in
// every composer state, after the panel grows (a quote strip, a multi-line draft, an edit, a picture chip) and after the
// window is resized; messages go under the panel only when scrolled up.
TEST_F(WideFrostLayoutTest, TheNewestMessageRestsAboveTheFloatingPanelInEveryState) {
  struct Case {
    const char* name;
    const char* primary_tab; // "ai" = the AI conversation (Home tab), else Chats
    bool group;
    void (*apply)(ChatScreenState&);
  };
  const Case cases[] = {
      {"chat", "chats", false, [](ChatScreenState&) {}},
      {"group", "chats", true, [](ChatScreenState&) {}},
      {"ai", "ai", false, [](ChatScreenState&) {}},
      {"reply strip", "chats", true,
       [](ChatScreenState& s) {
         s.quote_reply = true;
         s.quote_reply_text = "Earlier message";
       }},
      {"edit strip", "chats", false,
       [](ChatScreenState& s) {
         s.editing_message = true;
         s.quote_reply_text = "Edit message";
       }},
      {"picture chip", "chats", true, [](ChatScreenState& s) { s.image_chip = true; }},
      {"stop key", "ai", false, [](ChatScreenState& s) { s.loading = true; }},
      {"multi-line draft", "chats", false, [](ChatScreenState& s) { s.draft = "one\ntwo\nthree\nfour"; }},
      {"recording", "chats", false,
       [](ChatScreenState& s) {
         s.voice_recording = true;
         s.voice_timer = "0:03";
       }},
  };
  for (const float ratio : {1.f, 2.f}) {
    for (const Case& c : cases) {
      SCOPED_TRACE(std::string(c.name) + " ratio " + std::to_string(ratio));
      context_->SetDensityIndependentPixelRatio(ratio);
      ChatScreenState fresh = chat_;
      for (MessageDisplayRow& row : fresh.messages) {
        row.group_column = c.group;
      }
      c.apply(fresh);
      chat_ = fresh;
      Load(Wide(std::string(c.primary_tab) == "ai" ? NavTab::Home : NavTab::Sessions, "chat"));
      Frames::Run(context_, document_, /*pinned=*/true);
      const ComposerParts p = PartsOf(document_);
      ASSERT_TRUE(p.material);
      ui::Element* messages = First("#chat-messages");
      ui::ElementList rows;
      messages->GetElementsByClassName(rows, "message-row");
      ui::Element* last = nullptr;
      for (ui::Element* row : rows) {
        if (Height(row) > 0.f) {
          last = row;
        }
      }
      ASSERT_NE(last, nullptr);
      const float gap_dp = (Top(p.material) - Bottom(last)) / ratio;
      EXPECT_GE(gap_dp, 8.f) << "the newest message must not sit under the panel";
      EXPECT_LE(gap_dp, 40.f) << "and not leave a hole above it";
    }
  }
}

TEST_F(WideFrostLayoutTest, ThePanelGrowingOrTheWindowResizingKeepsTheNewestMessageClear) {
  Load(Wide(NavTab::Sessions, "chat"));
  Frames::Run(context_, document_, true);
  const auto gap = [&]() {
    ui::Element* messages = First("#chat-messages");
    ui::ElementList rows;
    messages->GetElementsByClassName(rows, "message-row");
    ui::Element* last = nullptr;
    for (ui::Element* row : rows) {
      if (Height(row) > 0.f) {
        last = row;
      }
    }
    return Top(PartsOf(document_).material) - Bottom(last);
  };
  EXPECT_GE(gap(), 8.f);
  const float plain_panel = Height(PartsOf(document_).material);
  // The panel grows in place: a quote strip, then a long draft.
  chat_.quote_reply = true;
  chat_.quote_reply_text = "Earlier message";
  DataModelHost::Instance().Dirty("chat", "quote_reply");
  DataModelHost::Instance().Dirty("chat", "quote_reply_text");
  chat_.draft = "one\ntwo\nthree\nfour";
  DataModelHost::Instance().Dirty("chat", "draft");
  Frames::Run(context_, document_, true);
  EXPECT_GT(Height(PartsOf(document_).material), plain_panel) << "the panel grew";
  EXPECT_GE(gap(), 8.f) << "after the panel grew";
  // And back.
  chat_.quote_reply = false;
  chat_.draft = "";
  DataModelHost::Instance().Dirty("chat", "quote_reply");
  DataModelHost::Instance().Dirty("chat", "draft");
  Frames::Run(context_, document_, true);
  EXPECT_NEAR(Height(PartsOf(document_).material), plain_panel, 0.5f);
  EXPECT_GE(gap(), 8.f);
  EXPECT_LE(gap(), 40.f);
  // The window resized (a reload at each size: the column is the window's width and height).
  for (const std::pair<int, int> size : {std::pair<int, int>{1340, 700}, {900, 560}, {1600, 1000}}) {
    window_w_ = size.first;
    window_h_ = size.second;
    Load(Wide(NavTab::Sessions, "chat"));
    Frames::Run(context_, document_, true);
    EXPECT_GE(gap(), 8.f) << size.first << "x" << size.second;
    EXPECT_LE(gap(), 40.f) << size.first << "x" << size.second;
  }
}

// Passing under the panel is for scrolling up: with the transcript scrolled away from the bottom, the rows reach under the
// panel's top edge (the panel is a floating one, not a bar the list stops at).
TEST_F(WideFrostLayoutTest, ScrolledUpTheMessagesRunUnderThePanel) {
  Load(Wide(NavTab::Sessions, "chat"));
  Frames::Run(context_, document_, true);
  ui::Element* messages = First("#chat-messages");
  messages->SetScrollTop(messages->GetScrollTop() - 300.f);
  context_->Update();
  const ComposerParts p = PartsOf(document_);
  ui::ElementList rows;
  messages->GetElementsByClassName(rows, "message-row");
  bool under = false;
  for (ui::Element* row : rows) {
    if (Height(row) > 0.f && Top(row) < Top(p.material) && Bottom(row) > Top(p.material) + 8.f) {
      under = true;
    }
  }
  EXPECT_TRUE(under) << "a row passes under the panel's top edge while scrolling up";
}

} // namespace
} // namespace pbr
