// Phone minimal AI Home (DEV/PLAN-minimal-nav.md B): the real stylesheets over the real markup of the centre block
// (logo, field mount, chips), laid out in the area left above the keyboard. The field must stay inside it, the logo
// is 100dp (72dp on a screen under 600dp tall), and the chips wrap onto more rows when they do not fit.
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
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

/** The real `.home-landing-inner` block of home.rml with its data bindings stripped and a stub field in the mount. */
std::string CentreBlock(bool mascot = false) {
  const std::string home = Asset("views/home.rml");
  const size_t begin = home.find("<div class=\"home-landing-inner");
  const size_t end = home.find("<div class=\"home-dock-seam\">");
  std::string block = home.substr(begin, end - begin);
  block = std::regex_replace(block, std::regex("<!--[\\s\\S]*?-->"), "");
  block = std::regex_replace(block, std::regex("\\s+data-[a-z-]+=\"[^\"]*\""), "");
  block = std::regex_replace(block, std::regex("\\{\\{[^}]*\\}\\}"), "");
  // Five chips of 110dp: two fit per row on a 320dp screen, three on 390dp.
  block = std::regex_replace(block, std::regex("<button class=\"chat-suggestion\""),
                             "<button class=\"chat-suggestion\" style=\"width: 110dp;\"");
  const std::string stub =
      "<div class=\"prompt-composer\" id=\"stub-composer\"><div class=\"prompt-composer-row\">"
      "<button class=\"prompt-voice-toggle-btn prompt-voice-toggle-btn--end\" id=\"stub-mic\"></button>"
      "<button class=\"prompt-emoji-btn\" id=\"stub-emoji\"><svg class=\"prompt-icon-v2\"></svg>"
      "<svg class=\"prompt-icon-home-emoji\"></svg></button>"
      "<button class=\"prompt-send-btn prompt-send-btn--idle\" id=\"stub-send\" disabled=\"disabled\">"
      "<span class=\"prompt-send-disc\"></span></button>"
      "<div class=\"prompt-composer-pill\"><textarea class=\"prompt-composer-input\"></textarea>"
      "<button class=\"prompt-home-plus\" id=\"stub-plus\"></button></div></div></div>";
  if (mascot) {
    const std::string logo =
        "<div class=\"home-logo\" id=\"home-logo\"><img src=\"../branding/app-icon.png\" alt=\"\"/></div>";
    const size_t at_logo = block.find("<div class=\"home-logo\"");
    const size_t end_logo = block.find("</div>", at_logo) + 6;
    block.replace(at_logo, end_logo - at_logo,
                  "<div class=\"home-logo home-logo--mascot\" id=\"home-logo\"><div class=\"home-mascot ai-dot\" "
                  "id=\"stub-mascot\"></div></div>");
    (void)logo;
  }
  const std::string mount = "id=\"home-composer-mount\">";
  const size_t at = block.find(mount);
  block.insert(at + mount.size(), stub);
  return block;
}

struct Box {
  float top, height;
  float bottom() const { return top + height; }
};

class HomeMinimalLayoutTest : public testing::Test {
protected:
  void TearDown() override {
    if (context) {
      ui::RemoveContext("home-minimal-layout-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  /** `visible` is the screen's height less the keyboard: the page ends there, as shell-root does. */
  void Load(int width, int screen_height, int keyboard, bool docked = false, bool mascot = false) {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("home-minimal-layout-test", {width, screen_height});
    ASSERT_NE(context, nullptr);
    context->ActivateTheme("light", true);
    const int visible = screen_height - keyboard;
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0;\">"
        "<div class=\"shell-layer-compact\" style=\"position: absolute; top: 0dp; left: 0dp; width: " +
        std::to_string(width) + "dp; height: " + std::to_string(visible) +
        "dp; display: flex; flex-direction: column;\">"
        // Under the 44dp bar and the status bar.
        "<div style=\"height: 90dp; flex: none;\"></div>"
        "<div class=\"home-landing home-landing--minimal\"" +
        // Docked: the landing keeps its undocked height (negative bottom margin = the keyboard's lift), as ShellHost does.
        std::string(docked ? " style=\"margin-bottom: -" + std::to_string(keyboard) + "dp;\"" : "") + ">" +
        CentreBlock(mascot) + "</div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
    visible_height = static_cast<float>(visible);
  }

  static Box BoxOf(ui::Element* element) {
    return {element->GetAbsoluteOffset(ui::BoxArea::Border).y, element->GetBox().GetSize(ui::BoxArea::Border).y};
  }
  Box ById(const char* id) {
    ui::Element* element = document->GetElementById(id);
    EXPECT_NE(element, nullptr) << id;
    return element ? BoxOf(element) : Box{0, 0};
  }
  Box First(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    EXPECT_FALSE(found.empty()) << cls;
    return found.empty() ? Box{0, 0} : BoxOf(found.front());
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  float visible_height = 0;
};

// iPhone 14 class, no keyboard: 100dp logo, the field two lines (76dp) tall, everything on screen.
TEST_F(HomeMinimalLayoutTest, TallScreenShowsLogoAndTwoLineField) {
  Load(390, 844, 0);
  EXPECT_FLOAT_EQ(First("home-logo").height, 100.f);
  const Box pill = First("prompt-composer-pill");
  EXPECT_GE(pill.height, 106.f);
  EXPECT_LE(pill.bottom(), visible_height);
  EXPECT_GT(First("home-logo").top, 90.f - 1.f) << "the logo is below the bar";
}

// Card B3 (mockups/31-home-composer-b.html, Kenneth 2026-11-09): 40dp side margins (310dp on a 390dp screen), 108dp
// tall, the chips under it on the same 310dp, 12dp between the logo box and the card, the emoji key a 32dp circle at the
// right end, and no microphone on the card.
TEST_F(HomeMinimalLayoutTest, CardIsTheNarrow108dpB3WithACircleEmojiAndNoMic) {
  Load(390, 844, 0);
  ui::Element* card = document->GetElementById("stub-composer");
  ASSERT_NE(card, nullptr);
  EXPECT_FLOAT_EQ(card->GetBox().GetSize(ui::BoxArea::Border).x, 310.f);
  EXPECT_FLOAT_EQ(card->GetAbsoluteOffset(ui::BoxArea::Border).x, 40.f);
  EXPECT_FLOAT_EQ(card->GetBox().GetSize(ui::BoxArea::Border).y, 108.f);
  const Box logo = First("home-logo");
  EXPECT_FLOAT_EQ(BoxOf(card).top - logo.bottom(), 12.f) << "logo box to card (was 24dp)";
  ui::ElementList chips;
  document->GetElementsByClassName(chips, "chat-suggestion");
  ASSERT_EQ(chips.size(), 5u);
  EXPECT_FLOAT_EQ(BoxOf(chips.front()).top - BoxOf(card).bottom(), 16.f) << "card to the first chip row";
  EXPECT_GE(chips.front()->GetAbsoluteOffset(ui::BoxArea::Border).x, 40.f);
  ui::Element* mic = document->GetElementById("stub-mic");
  ASSERT_NE(mic, nullptr);
  EXPECT_EQ(mic->GetComputedValues().display(), ui::Style::Display::None) << "no microphone on the Home card";
  ui::Element* emoji = document->GetElementById("stub-emoji");
  ASSERT_NE(emoji, nullptr);
  // A 44dp target; the 32dp tinted circle is drawn on the icon inside it (its centre 32dp from the card's right edge).
  const ui::Vector2f size = emoji->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(size.x, 44.f);
  EXPECT_FLOAT_EQ(size.y, 44.f);
  const float card_right = card->GetAbsoluteOffset(ui::BoxArea::Border).x + 310.f;
  const float emoji_right = card_right - (emoji->GetAbsoluteOffset(ui::BoxArea::Border).x + size.x);
  EXPECT_FLOAT_EQ(emoji_right, 51.f) << "right: 50dp + border, the tinted circle 10dp before the Send disc";
  // Send is always at the right end: a 36dp disc (idle while empty), nothing swaps places.
  ui::Element* send = document->GetElementById("stub-send");
  ASSERT_NE(send, nullptr);
  EXPECT_NE(send->GetComputedValues().display(), ui::Style::Display::None) << "Send is always present";
  const ui::Vector2f send_size = send->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(send_size.x, 36.f);
  EXPECT_FLOAT_EQ(send_size.y, 36.f);
  EXPECT_FLOAT_EQ(card_right - (send->GetAbsoluteOffset(ui::BoxArea::Border).x + send_size.x), 11.f)
      << "right end (10dp + border)";
  EXPECT_LT(emoji->GetAbsoluteOffset(ui::BoxArea::Border).x + size.x, send->GetAbsoluteOffset(ui::BoxArea::Border).x)
      << "the emoji key is left of Send";
  ui::Element* plus = document->GetElementById("stub-plus");
  ASSERT_NE(plus, nullptr);
  EXPECT_LT(plus->GetAbsoluteOffset(ui::BoxArea::Border).x + plus->GetBox().GetSize(ui::BoxArea::Border).x,
            emoji->GetAbsoluteOffset(ui::BoxArea::Border).x)
      << "the plus stays at the far left, before the emoji key";
  EXPECT_FLOAT_EQ(plus->GetAbsoluteOffset(ui::BoxArea::Border).x - card->GetAbsoluteOffset(ui::BoxArea::Border).x, 5.f)
      << "the plus keeps left: 4dp + border";
}

TEST_F(HomeMinimalLayoutTest, KeyboardGoneCentresTheBlockAgain) {
  Load(390, 844, 0, false);
  const Box card = ById("stub-composer");
  const Box logo = First("home-logo");
  std::printf("[844/0 centred] card top %.1f bottom %.1f logo h %.1f top %.1f\n", card.top, card.bottom(), logo.height,
              logo.top);
  EXPECT_FLOAT_EQ(logo.height, 100.f);
  EXPECT_LT(card.bottom(), visible_height - 60.f) << "not at the bottom any more";
  EXPECT_GT(logo.top, 90.f + 40.f) << "the block is centred, the logo not pinned to the top";
}

TEST_F(HomeMinimalLayoutTest, SmallScreenWithoutKeyboardUsesTheSmallLogoAndWrapsChips) {
  Load(320, 568, 0);
  EXPECT_FLOAT_EQ(First("home-logo").height, 72.f);
  ui::ElementList chips;
  document->GetElementsByClassName(chips, "chat-suggestion");
  ASSERT_EQ(chips.size(), 5u);
  float rows_top = BoxOf(chips.front()).top;
  float last_top = BoxOf(chips.back()).top;
  EXPECT_GT(last_top, rows_top) << "chips wrap onto more than one row";
  EXPECT_LE(First("prompt-composer-pill").bottom(), visible_height);
}

TEST_F(HomeMinimalLayoutTest, KeyboardUpOnTallScreenKeepsFieldAboveTheKeyboard) {
  Load(390, 844, 336);
  const Box pill = First("prompt-composer-pill");
  EXPECT_LE(pill.bottom(), visible_height);
  EXPECT_GE(pill.top, 90.f);
}

// One document, three states (the phone: tap the field, the keyboard / emoji sheet rises, then goes away): the layer
// is shortened by the lift the way ShellHost lifts #shell-root, and the block must come back to where it was.
TEST_F(HomeMinimalLayoutTest, BlockReturnsWhenTheKeyboardOrEmojiSheetGoesAway) {
  for (const int lift : {336, 291}) {
    SCOPED_TRACE(lift);
    if (context) {
      ui::RemoveContext("home-minimal-layout-test");
      context = nullptr;
      ui::Shutdown();
      initialized = false;
    }
    Load(390, 844, 0, false, true);
    ui::ElementList landings;
    document->GetElementsByClassName(landings, "home-landing");
    ASSERT_FALSE(landings.empty());
    ui::Element* layer = landings.front()->GetParentNode();
    const auto set_lift = [&](int dp) {
      layer->SetProperty("height", (std::to_string(844 - dp) + "dp").c_str());
      context->Update();
      context->Update();
    };
    const Box card0 = ById("stub-composer");
    const Box logo0 = First("home-logo");
    set_lift(lift);
    const Box card1 = ById("stub-composer");
    std::printf("[lift %d] card %.1f -> %.1f -> ", lift, card0.top, card1.top);
    EXPECT_NE(card1.top, card0.top) << "the block re-centres in the remaining space";
    EXPECT_LE(card1.bottom(), 844.f - lift);
    set_lift(0);
    const Box card2 = ById("stub-composer");
    const Box logo2 = First("home-logo");
    std::printf("%.1f (logo %.1f -> %.1f)\n", card2.top, logo0.top, logo2.top);
    EXPECT_FLOAT_EQ(card2.top, card0.top);
    EXPECT_FLOAT_EQ(logo2.top, logo0.top);
  }
}

// The Chats header (sidebar.rml's .pane-nav) on a phone: a fixed 56dp bar holding the compose button at the right (and
// the centred small title once the list has scrolled), and the scroller starting at the very top with one 56dp title
// row ("Chats", the link status as a dot and a word) in the band under the bar, then the search box. Bindings stripped.
class ChatsHeaderTest : public HomeMinimalLayoutTest {
protected:
  void LoadChats(const int top_inset_dp) {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("home-minimal-layout-test", {390, 844});
    ASSERT_NE(context, nullptr);
    context->ActivateTheme("light", true);
    const std::string sidebar = Asset("views/sidebar.rml");
    const size_t bar_begin = sidebar.find("<div class=\"pane-nav\">");
    std::string bar = sidebar.substr(bar_begin, sidebar.find("<div class=\"sidebar-header row\">") - bar_begin);
    bar = std::regex_replace(bar, std::regex("<!--[\\s\\S]*?-->"), "");
    bar = std::regex_replace(bar, std::regex("\\s+data-[a-z-]+=\"[^\"]*\""), "");
    bar = std::regex_replace(bar, std::regex("\\{\\{[^}]*\\}\\}"), "C");
    const size_t title_begin = sidebar.find("<div class=\"large-title\" data-model=\"window\">");
    std::string title = sidebar.substr(title_begin, sidebar.find("<div class=\"chat-search\">") - title_begin);
    title = std::regex_replace(title, std::regex("<!--[\\s\\S]*?-->"), "");
    title = std::regex_replace(title, std::regex("\\s+data-[a-z-]+=\"[^\"]*\""), "");
    title = std::regex_replace(title, std::regex("\\{\\{[^}]*\\}\\}"), "C");
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0;\">"
        "<div class=\"shell-layer-compact\" style=\"position: absolute; left: 0dp; width: 390dp; height: 800dp; top: " +
        std::to_string(top_inset_dp) + "dp;\"><div class=\"sidebar-panel\">" + bar +
        "<div class=\"sidebar-sessions root-scroll\">" + title +
        "<div class=\"chat-search\"><input type=\"text\" class=\"chat-search-input\"/></div>"
        "</div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  ui::Element* One(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  }
  static ui::Vector2f SizeOf(ui::Element* e) { return e->GetBox().GetSize(ui::BoxArea::Border); }
};

TEST_F(ChatsHeaderTest, TitleRowIsOneRowWithTheTitleAndTheStatusDot) {
  const std::string rml = Asset("views/sidebar.rml");
  const size_t bar_end = rml.find("<div class=\"sidebar-header row\">");
  const std::string bar = rml.substr(0, bar_end);
  EXPECT_EQ(rml.find("pane-nav-me"), std::string::npos);
  EXPECT_EQ(rml.find("chat-me"), std::string::npos);
  EXPECT_EQ(rml.find("{{profile_nickname}}"), std::string::npos) << "no avatar, no name";
  EXPECT_EQ(rml.find("open_settings_from_avatar"), std::string::npos);
  EXPECT_NE(bar.find("<p class=\"pane-nav-title\">{{i18n:nav.sessions}}</p>"), std::string::npos)
      << "the small bar's centred title fades in once the row has scrolled away";
  EXPECT_NE(bar.find("open_new_session_menu()"), std::string::npos) << "the compose button is in the fixed bar";
  EXPECT_NE(bar.find("compose.svg"), std::string::npos);
  EXPECT_NE(rml.find("open_new_session_menu()", bar_end), std::string::npos)
      << "the desktop header keeps its own plus button";

  const size_t title = rml.find("<div class=\"large-title\" data-model=\"window\">", bar_end);
  const size_t search = rml.find("<div class=\"chat-search\">", bar_end);
  ASSERT_NE(title, std::string::npos);
  ASSERT_LT(title, search);
  const std::string row = rml.substr(title, search - title);
  const size_t word = row.find("<p class=\"large-title-text\">{{i18n:nav.sessions}}</p>");
  const size_t status = row.find("<p class=\"link-status\" data-if=\"chats_link_state != ''\">");
  ASSERT_NE(word, std::string::npos);
  ASSERT_NE(status, std::string::npos);
  EXPECT_LT(word, status) << "the title first, the status after it on the same row";
  EXPECT_NE(row.find("{{chats_link_text}}"), std::string::npos);
  EXPECT_NE(rml.find("<div id=\"sidebar-sessions\" class=\"sidebar-sessions root-scroll\" "
                     "data-event-scroll=\"sessions_scroll()\">"),
            std::string::npos)
      << "the default collapse threshold";
  EXPECT_EQ(rml.find("collapse-at"), std::string::npos);
  EXPECT_EQ(row.find("<svg"), std::string::npos) << "no cloud icons: dots only";
}

TEST_F(ChatsHeaderTest, TheThreeStatesAreThreeDotsWithTheirOwnColours) {
  const std::string rml = Asset("views/sidebar.rml");
  for (const auto& [state, cls] : {std::pair<const char*, const char*>{"ok", "link-dot--ok"},
                                   {"connecting", "link-dot--connecting"},
                                   {"offline", "link-dot--offline"}}) {
    const std::string dot =
        std::string("<span class=\"link-dot ") + cls + "\" data-if=\"chats_link_state == '" + state + "'\"></span>";
    EXPECT_NE(rml.find(dot), std::string::npos) << dot;
  }
  EXPECT_NE(rml.find("<p class=\"link-status\" data-if=\"chats_link_state != ''\">"), std::string::npos)
      << "an unknown state draws nothing";
  // Colours come from the theme tokens (theme_tokens.tsv): in light and in dark.
  for (const char* sheet : {"themes/colors-light.rcss", "themes/colors-dark.rcss"}) {
    const std::string css = Asset(sheet);
    for (const char* cls : {".link-dot--ok {", ".link-dot--connecting {", ".link-dot--offline {"}) {
      EXPECT_NE(css.find(cls), std::string::npos) << sheet << " " << cls;
    }
    EXPECT_EQ(css.find("ai-drawer-me-link-icon"), std::string::npos);
  }
  const std::string tmpl = Asset("themes/colors.rcss.tmpl");
  const auto token_of = [&](const char* cls) {
    const size_t rule = tmpl.find(cls);
    const size_t open = tmpl.find("{{", rule);
    return tmpl.substr(open, tmpl.find("}}", open) + 2 - open);
  };
  EXPECT_EQ(token_of(".link-dot--ok {"), "{{success}}");
  EXPECT_EQ(token_of(".link-dot--connecting {"), "{{warn}}");
  EXPECT_EQ(token_of(".link-dot--offline {"), "{{gray}}");
}

TEST_F(ChatsHeaderTest, SmallBarHoldsTheTitleAndCompose) {
  LoadChats(0);
  ui::Element* title = One("pane-nav-title");
  ui::Element* button = One("pane-nav-btn");
  ASSERT_NE(title, nullptr);
  ASSERT_NE(button, nullptr);
  EXPECT_EQ(title->GetComputedValues().opacity(), 0.f) << "clear until the large title has scrolled away";
  EXPECT_FLOAT_EQ(title->GetAbsoluteOffset(ui::BoxArea::Border).x, 96.f);
  EXPECT_FLOAT_EQ(SizeOf(title).x, 390.f - 192.f) << "centred between the two 96dp margins";
}

TEST_F(ChatsHeaderTest, TheFixedBarLetsTouchesThroughExceptOnItsButton) {
  LoadChats(0);
  for (const char* cls : {"pane-nav", "pane-nav-bg", "bar-fade", "pane-nav-title"}) {
    ui::Element* e = One(cls);
    ASSERT_NE(e, nullptr) << cls;
    EXPECT_EQ(e->GetComputedValues().pointer_events(), ui::Style::PointerEvents::None)
        << cls << ": a drag that starts on the title row must reach the list";
  }
  for (const char* cls : {"pane-nav-actions", "pane-nav-btn"}) {
    ui::Element* e = One(cls);
    ASSERT_NE(e, nullptr) << cls;
    EXPECT_EQ(e->GetComputedValues().pointer_events(), ui::Style::PointerEvents::Auto) << cls;
  }
  // A press on the title row lands on the list (the scroller's own title row), not on the bar above it.
  ui::Element* title = One("large-title");
  ASSERT_NE(title, nullptr);
  const ui::Vector2f at = title->GetAbsoluteOffset(ui::BoxArea::Border);
  ui::Element* hit = context->GetElementAtPoint({at.x + 40.f, at.y + 28.f});
  ASSERT_NE(hit, nullptr);
  bool under_title_row = false;
  for (ui::Element* node = hit; node; node = node->GetParentNode()) {
    under_title_row = under_title_row || node == title;
  }
  EXPECT_TRUE(under_title_row) << "the touch went through the bar";
}

TEST_F(ChatsHeaderTest, ComposeButtonSitsAtTheTopRightAndDoesNotMoveWhenTheTitleCollapses) {
  LoadChats(0);
  ui::Element* button = One("pane-nav-btn");
  ASSERT_NE(button, nullptr);
  EXPECT_FLOAT_EQ(SizeOf(button).x, 44.f);
  EXPECT_FLOAT_EQ(SizeOf(button).y, 44.f);
  const ui::Vector2f at = button->GetAbsoluteOffset(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(at.x + SizeOf(button).x, 390.f - 12.f) << "12dp from the right edge";
  EXPECT_LT(at.y, 56.f) << "inside the bar";
  // The collapse only fades the bar's background and title in; nothing moves the button.
  const std::string css = Asset("themes/components.rcss");
  EXPECT_EQ(css.find(".large-title-collapsed .pane-nav-actions"), std::string::npos);
  EXPECT_EQ(css.find(".large-title-collapsed .pane-nav-btn"), std::string::npos);
}

TEST_F(ChatsHeaderTest, TheTitleRowIsTheBandUnderTheBarAndTheSearchBoxFollowsIt) {
  for (const int inset : {0, 47}) {
    SCOPED_TRACE(inset);
    if (context) {
      ui::RemoveContext("home-minimal-layout-test");
      context = nullptr;
      ui::Shutdown();
      initialized = false;
    }
    LoadChats(inset);
    ui::Element* bar = One("pane-nav");
    ui::Element* scroller = One("sidebar-sessions");
    ui::Element* title = One("large-title");
    ui::Element* word = One("large-title-text");
    ui::Element* status = One("link-status");
    ui::Element* search = One("chat-search");
    ui::Element* button = One("pane-nav-btn");
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(scroller, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NE(word, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(search, nullptr);
    ASSERT_NE(button, nullptr);
    EXPECT_FLOAT_EQ(SizeOf(bar).y, 56.f);
    EXPECT_FLOAT_EQ(scroller->GetComputedValues().padding_top().value, 0.f) << "the list starts at the top";
    EXPECT_FLOAT_EQ(SizeOf(title).y, 56.f) << "one 56dp row: the same band as the bar";
    EXPECT_FLOAT_EQ(title->GetAbsoluteOffset(ui::BoxArea::Border).y, bar->GetAbsoluteOffset(ui::BoxArea::Border).y);
    EXPECT_FLOAT_EQ(word->GetAbsoluteOffset(ui::BoxArea::Border).x, 16.f);
    EXPECT_GT(status->GetAbsoluteOffset(ui::BoxArea::Border).x,
              word->GetAbsoluteOffset(ui::BoxArea::Border).x + SizeOf(word).x - 0.5f)
        << "the status follows the title on the same row";
    EXPECT_LE(status->GetAbsoluteOffset(ui::BoxArea::Border).x + SizeOf(status).x, 390.f - 56.f + 0.5f)
        << "clear of the compose button's 56dp";
    // The compose icon's centre is 28dp (2dp + half of 52dp); the title's line box is 41dp with 7dp under it in the
    // 56dp row, so its centre is 56 - 7 - 20.5 = 28.5dp (the test has no font, so the box height cannot be measured).
    EXPECT_FLOAT_EQ(word->GetComputedValues().margin_bottom().value, 7.f);
    EXPECT_FLOAT_EQ(word->GetComputedValues().line_height().value, 41.f);
    EXPECT_NEAR(56.f - 7.f - 41.f / 2.f,
                button->GetAbsoluteOffset(ui::BoxArea::Border).y - bar->GetAbsoluteOffset(ui::BoxArea::Border).y +
                    SizeOf(button).y / 2.f,
                1.f)
        << "the compose icon is centred on the title";
    EXPECT_GE(search->GetAbsoluteOffset(ui::BoxArea::Border).y,
              title->GetAbsoluteOffset(ui::BoxArea::Border).y + SizeOf(title).y - 0.5f)
        << "the search box is under the title row";
    EXPECT_FLOAT_EQ(SizeOf(search).y, 36.f);
    EXPECT_FLOAT_EQ(search->GetAbsoluteOffset(ui::BoxArea::Border).x, 20.f);
  }
}

} // namespace
} // namespace pbr
