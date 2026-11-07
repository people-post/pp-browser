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

// The Chats bar (sidebar.rml's .pane-nav) on a phone: the 52dp avatar and the name beside it are on screen, inside the
// 56dp bar, with the name's text drawn. Bindings stripped (the name is a stand-in word).
TEST_F(HomeMinimalLayoutTest, ChatsBarShowsTheAvatarAndTheName) {
  ui::SetRenderInterface(&renderer);
  initialized = ui::Initialise();
  ASSERT_TRUE(initialized);
  context = ui::CreateContext("home-minimal-layout-test", {390, 844});
  ASSERT_NE(context, nullptr);
  context->ActivateTheme("light", true);
  const std::string sidebar = Asset("views/sidebar.rml");
  std::string bar = sidebar.substr(sidebar.find("<div class=\"pane-nav\">"),
                                   sidebar.find("<div class=\"sidebar-header row\">") - sidebar.find("<div class=\"pane-nav\">"));
  bar = std::regex_replace(bar, std::regex("<!--[\\s\\S]*?-->"), "");
  bar = std::regex_replace(bar, std::regex("\\s+data-[a-z-]+=\"[^\"]*\""), "");
  bar = std::regex_replace(bar, std::regex("\\{\\{profile_nickname\\}\\}"), "Kenneth");
  bar = std::regex_replace(bar, std::regex("\\{\\{[^}]*\\}\\}"), "K");
  const std::string rml =
      "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
      Asset("themes/colors-light.rcss") + "</style></head><body style=\"padding: 0;\">"
      "<div class=\"shell-layer-compact\" style=\"position: absolute; top: 0dp; left: 0dp; width: 390dp; height: 800dp;\">"
      "<div class=\"sidebar-panel\">" + bar + "</div></div></body></rml>";
  document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Update();
  ui::ElementList found;
  document->GetElementsByClassName(found, "pane-nav-avatar");
  ASSERT_FALSE(found.empty());
  ui::Element* avatar = found.front();
  const ui::Vector2f size = avatar->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_GE(size.x, 52.f) << "avatar plus name";
  EXPECT_FLOAT_EQ(size.y, 52.f);
  EXPECT_FLOAT_EQ(avatar->GetAbsoluteOffset(ui::BoxArea::Border).x, 16.f);
  EXPECT_LE(avatar->GetAbsoluteOffset(ui::BoxArea::Border).y + size.y, 56.f + 2.f);
  ui::ElementList names;
  document->GetElementsByClassName(names, "pane-nav-avatar-name");
  ASSERT_FALSE(names.empty());
  EXPECT_GT(names.front()->GetBox().GetSize(ui::BoxArea::Border).x, 20.f) << "the name has a width";
  ui::ElementList faces;
  document->GetElementsByClassName(faces, "shell-circle-avatar");
  ASSERT_FALSE(faces.empty());
  EXPECT_FLOAT_EQ(faces.front()->GetBox().GetSize(ui::BoxArea::Border).x, 52.f);
}

} // namespace
} // namespace pbr
