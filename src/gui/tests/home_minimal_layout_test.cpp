// Phone minimal AI Home (DEV/PLAN-minimal-nav.md B): the real stylesheets over the real markup of the centre block
// (logo, field mount, chips), laid out in the area left above the keyboard. The field must stay inside it, the logo
// is 100dp (72dp on a screen under 600dp tall), and the chips wrap onto more rows when they do not fit.
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

/** The real `.home-landing-inner` block of home.rml with its data bindings stripped and a stub field in the mount. */
std::string CentreBlock() {
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
      "<div class=\"prompt-composer-pill\"><textarea class=\"prompt-composer-input\"></textarea></div></div></div>";
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
    if (context) { ui::RemoveContext("home-minimal-layout-test"); }
    if (initialized) { ui::Shutdown(); }
    ui::SetRenderInterface(nullptr);
  }

  /** `visible` is the screen's height less the keyboard: the page ends there, as shell-root does. */
  void Load(int width, int screen_height, int keyboard) {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("home-minimal-layout-test", {width, screen_height});
    ASSERT_NE(context, nullptr);
    context->ActivateTheme("light", true);
    const int visible = screen_height - keyboard;
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/colors-light.rcss") + "</style></head><body style=\"padding: 0;\">"
        "<div class=\"shell-layer-compact\" style=\"position: absolute; top: 0dp; left: 0dp; width: " +
        std::to_string(width) + "dp; height: " + std::to_string(visible) +
        "dp; display: flex; flex-direction: column;\">"
        // Under the 44dp bar and the status bar.
        "<div style=\"height: 90dp; flex: none;\"></div>"
        "<div class=\"home-landing home-landing--minimal\">" + CentreBlock() + "</div></div></body></rml>";
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
  EXPECT_GE(pill.height, 76.f);
  EXPECT_LE(pill.bottom(), visible_height);
  EXPECT_GT(First("home-logo").top, 90.f - 1.f) << "the logo is below the bar";
}

// With the keyboard up (about 260dp on a 568dp screen, 336dp on a 844dp one) the field stays inside what is left,
// on top of the keyboard; the logo and chips give way (they shrink and clip) rather than push it out.
TEST_F(HomeMinimalLayoutTest, KeyboardUpOnSmallScreenKeepsFieldAboveTheKeyboard) {
  Load(320, 568, 260);
  EXPECT_FLOAT_EQ(First("home-logo").height <= 72.f ? 1.f : 0.f, 1.f) << "72dp logo (or less) under 600dp";
  const Box pill = First("prompt-composer-pill");
  EXPECT_GE(pill.top, 90.f) << "below the bar";
  EXPECT_LE(pill.bottom(), visible_height) << "the field's bottom is inside the area above the keyboard";
  const Box landing = First("home-landing");
  EXPECT_LE(landing.bottom(), visible_height);
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

// The Chats header (sidebar.rml's .pane-nav, B3) on a phone: a 64dp avatar 20dp from the left and 20dp below the top, the
// name at 22dp with the status line under it, the teal plus centred on the avatar, the whole header 104dp, and the
// scroller's first row (the search box) starting at or below its bottom edge, with and without a notch above the page.
// Bindings stripped (the name is a stand-in word).
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
    bar = std::regex_replace(bar, std::regex("\\{\\{profile_nickname\\}\\}"), "Kenneth");
    bar = std::regex_replace(bar, std::regex("\\{\\{chats_link_text\\}\\}"), "Online");
    bar = std::regex_replace(bar, std::regex("\\{\\{[^}]*\\}\\}"), "K");
    const std::string rml =
        "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
        Asset("themes/colors-light.rcss") + "</style></head><body style=\"padding: 0;\">"
        "<div class=\"shell-layer-compact\" style=\"position: absolute; left: 0dp; width: 390dp; height: 800dp; top: " +
        std::to_string(top_inset_dp) + "dp;\"><div class=\"sidebar-panel\">" + bar +
        "<div class=\"sidebar-sessions root-scroll\"><div class=\"chat-search\"><input type=\"text\" class=\"chat-search-input\"/></div>"
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

TEST_F(ChatsHeaderTest, ShowsTheBigAvatarTheNameAndTheStatusLine) {
  LoadChats(0);
  ui::Element* avatar = One("pane-nav-avatar");
  ASSERT_NE(avatar, nullptr);
  EXPECT_FLOAT_EQ(SizeOf(avatar).y, 64.f);
  EXPECT_GE(SizeOf(avatar).x, 64.f) << "avatar plus the name column";
  EXPECT_FLOAT_EQ(avatar->GetAbsoluteOffset(ui::BoxArea::Border).x, 20.f);
  EXPECT_FLOAT_EQ(avatar->GetAbsoluteOffset(ui::BoxArea::Border).y, 20.f);
  ui::Element* face = One("shell-circle-avatar");
  ASSERT_NE(face, nullptr);
  EXPECT_FLOAT_EQ(SizeOf(face).x, 64.f);
  EXPECT_FLOAT_EQ(SizeOf(face).y, 64.f);
  ui::Element* name = One("pane-nav-avatar-name");
  ASSERT_NE(name, nullptr);
  EXPECT_GT(SizeOf(name).x, 20.f) << "the name has a width";
  EXPECT_FLOAT_EQ(name->GetComputedValues().font_size(), 22.f);
  ui::Element* sub = One("pane-nav-avatar-sub");
  ASSERT_NE(sub, nullptr) << "the status line exists";
  EXPECT_GT(SizeOf(sub).x, 14.f);
  EXPECT_FLOAT_EQ(sub->GetComputedValues().font_size(), 13.f);
  EXPECT_GE(sub->GetAbsoluteOffset(ui::BoxArea::Border).y, name->GetAbsoluteOffset(ui::BoxArea::Border).y + SizeOf(name).y - 1.f)
      << "the status line sits under the name";
  const float avatar_bottom = avatar->GetAbsoluteOffset(ui::BoxArea::Border).y + SizeOf(avatar).y;
  EXPECT_LE(sub->GetAbsoluteOffset(ui::BoxArea::Border).y + SizeOf(sub).y, avatar_bottom + 1.f) << "name + status stay in the avatar's height";
}

TEST_F(ChatsHeaderTest, PlusIsCentredOnTheAvatarTwentyFromTheEdge) {
  LoadChats(0);
  ui::Element* face = One("pane-nav-plus");
  ASSERT_NE(face, nullptr);
  const ui::Vector2f at = face->GetAbsoluteOffset(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(SizeOf(face).x, 52.f) << "hit area";
  ui::Element* disc = One("shell-circle-btn-face");
  ASSERT_NE(disc, nullptr);
  EXPECT_FLOAT_EQ(SizeOf(disc).x, 44.f);
  const ui::Vector2f disc_at = disc->GetAbsoluteOffset(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(disc_at.x + SizeOf(disc).x, 390.f - 20.f) << "20dp from the right edge";
  EXPECT_FLOAT_EQ(disc_at.y + SizeOf(disc).y / 2.f, 20.f + 32.f) << "centred on the 64dp avatar";
  (void)at;
}

TEST_F(ChatsHeaderTest, HeaderIs104dpAndTheSearchBoxStartsBelowIt) {
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
    ui::Element* frost = One("pane-nav-bg");
    ui::Element* scroller = One("sidebar-sessions");
    ui::Element* search = One("chat-search");
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(frost, nullptr);
    ASSERT_NE(scroller, nullptr);
    ASSERT_NE(search, nullptr);
    EXPECT_FLOAT_EQ(SizeOf(bar).y, 104.f);
    EXPECT_FLOAT_EQ(SizeOf(frost).y, 104.f) << "the frosted layer follows the same height";
    EXPECT_FLOAT_EQ(scroller->GetComputedValues().padding_top().value, 104.f) << "the list clears the header by the same amount";
    const float header_bottom = bar->GetAbsoluteOffset(ui::BoxArea::Border).y + SizeOf(bar).y;
    EXPECT_LE(header_bottom, search->GetAbsoluteOffset(ui::BoxArea::Border).y) << "the search box is not under the header";
    EXPECT_FLOAT_EQ(SizeOf(search).y, 36.f);
    EXPECT_FLOAT_EQ(search->GetAbsoluteOffset(ui::BoxArea::Border).x, 20.f);
  }
}

} // namespace
} // namespace pbr
