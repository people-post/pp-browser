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
      "<div class=\"prompt-composer-pill\"><textarea class=\"prompt-composer-input\"></textarea></div></div></div>";
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
  EXPECT_FLOAT_EQ(emoji->GetAbsoluteOffset(ui::BoxArea::Border).x - card->GetAbsoluteOffset(ui::BoxArea::Border).x,
                  43.f)
      << "left, right after the plus (42dp + border): the tinted circle 12dp from the plus glyph";
  // Send is always at the right end: a 36dp disc (idle while empty), nothing swaps places.
  ui::Element* send = document->GetElementById("stub-send");
  ASSERT_NE(send, nullptr);
  EXPECT_NE(send->GetComputedValues().display(), ui::Style::Display::None) << "Send is always present";
  const ui::Vector2f send_size = send->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(send_size.x, 36.f);
  EXPECT_FLOAT_EQ(send_size.y, 36.f);
  EXPECT_FLOAT_EQ(card_right - (send->GetAbsoluteOffset(ui::BoxArea::Border).x + send_size.x), 11.f)
      << "right end (10dp + border)";
  EXPECT_LT(emoji->GetAbsoluteOffset(ui::BoxArea::Border).x + size.x, send->GetAbsoluteOffset(ui::BoxArea::Border).x);
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

// The Chats header (sidebar.rml's .pane-nav) on a phone: a 56dp bar holding the compose button at the right, and the
// scroller's content under it starting with the large title ("Chats") and then the search box. Chrome rules: the avatar,
// the name and the link status are not on this page (they live in the AI drawer's "you" row). Bindings stripped.
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
    bar = std::regex_replace(bar, std::regex("\\{\\{[^}]*\\}\\}"), "Chats");
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0;\">"
        "<div class=\"shell-layer-compact\" style=\"position: absolute; left: 0dp; width: 390dp; height: 800dp; top: " +
        std::to_string(top_inset_dp) + "dp;\"><div class=\"sidebar-panel\">" + bar +
        "<div class=\"sidebar-sessions root-scroll\"><p class=\"large-title\">Chats</p>"
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

TEST_F(ChatsHeaderTest, HasNoAvatarNameOrStatusLine) {
  const std::string rml = Asset("views/sidebar.rml");
  const std::string bar = rml.substr(0, rml.find("<div class=\"sidebar-header row\">"));
  for (const char* gone :
       {"pane-nav-avatar", "shell-circle-avatar", "profile_nickname", "chats_link_state", "pane-nav-plus"}) {
    EXPECT_EQ(bar.find(gone), std::string::npos) << gone;
  }
  EXPECT_NE(bar.find("open_new_session_menu()"), std::string::npos);
  EXPECT_NE(bar.find("compose.svg"), std::string::npos);
}

TEST_F(ChatsHeaderTest, ComposeButtonSitsAtTheTopRight) {
  LoadChats(0);
  ui::Element* button = One("pane-nav-btn");
  ASSERT_NE(button, nullptr);
  EXPECT_FLOAT_EQ(SizeOf(button).x, 44.f);
  const ui::Vector2f at = button->GetAbsoluteOffset(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(at.x + SizeOf(button).x, 390.f - 12.f) << "12dp from the right edge";
  EXPECT_LT(at.y, 56.f) << "inside the bar";
}

TEST_F(ChatsHeaderTest, BarIs56dpAndTheLargeTitleThenTheSearchBoxScrollUnderIt) {
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
    ui::Element* search = One("chat-search");
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(scroller, nullptr);
    ASSERT_NE(title, nullptr);
    ASSERT_NE(search, nullptr);
    EXPECT_FLOAT_EQ(SizeOf(bar).y, 56.f);
    EXPECT_FLOAT_EQ(scroller->GetComputedValues().padding_top().value, 56.f) << "the list clears the bar";
    const float bar_bottom = bar->GetAbsoluteOffset(ui::BoxArea::Border).y + SizeOf(bar).y;
    EXPECT_GE(title->GetAbsoluteOffset(ui::BoxArea::Border).y, bar_bottom - 0.5f) << "the title starts below the bar";
    EXPECT_FLOAT_EQ(title->GetComputedValues().font_size(), 34.f);
    EXPECT_GE(search->GetAbsoluteOffset(ui::BoxArea::Border).y,
              title->GetAbsoluteOffset(ui::BoxArea::Border).y + SizeOf(title).y - 0.5f)
        << "the search box is under the title";
    EXPECT_FLOAT_EQ(SizeOf(search).y, 36.f);
    EXPECT_FLOAT_EQ(search->GetAbsoluteOffset(ui::BoxArea::Border).x, 20.f);
  }
}

} // namespace
} // namespace pbr
