// The wide-screen stylesheet (assets/themes/wide.rcss, desktop foundation step 1): it can only act under the wide root
// `.shell-body--wide`, so the phone's `.shell-body--compact` renders exactly as it did without the file; and under the
// wide root the three columns give way as designed (rail 64, list 280..., conversation >= 420) at 768 / 900 / 1280.
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <ui/style/ComputedValues.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <regex>
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

/** The elements the wide sheet touches, under one root class. Ids are what the measurements are keyed by. */
std::string Scene(const std::string& root_class, int width_dp) {
  return "<div id=\"root\" class=\"shell-body " + root_class +
         "\" style=\"position: absolute; top: 0; left: 0; width: " + std::to_string(width_dp) +
         "dp; height: 700dp;\">"
         "<div class=\"shell-chrome\"><div id=\"stack\" class=\"shell-toast-stack\"><div id=\"toast\" "
         "class=\"shell-toast\">"
         "<p class=\"shell-toast-text\">Copied</p></div></div></div>"
         "<div class=\"shell-layer shell-layer-base\"><div id=\"row\" class=\"shell-pane-row\">"
         "<div id=\"rail\" class=\"shell-nav-rail\"></div>"
         "<div id=\"list\" class=\"shell-pane shell-pane-secondary\" style=\"flex: 0 1 320dp;\"></div>"
         "<div id=\"conv\" class=\"shell-pane shell-pane-primary\">"
         "<button id=\"host\" class=\"tip-host\" type=\"button\">Go<span id=\"tip\" "
         "class=\"tip\">Explain</span></button>"
         "<div id=\"gap\" class=\"w-gap-8 w-type-small w-target\" style=\"display: "
         "flex;\"><span>a</span><span>b</span></div>"
         "</div></div></div>"
         "<div id=\"layer\" class=\"context-menu-layer\"><div id=\"context-menu-panel\" class=\"context-menu-panel\" "
         "style=\"left: 40px; top: 40px;\"><button id=\"item\" class=\"context-menu-item\" type=\"button\">"
         "<span class=\"context-menu-item-label\">Reply</span></button></div></div>"
         "</div>";
}

const char* const kIds[] = {"rail", "list", "conv", "host", "tip", "gap", "stack", "toast", "context-menu-panel",
                            "item"};

class WideStylesheetTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(
        ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/Inter-Regular.ttf", /*fallback_face=*/true));
    ASSERT_TRUE(ui::LoadFontFace(std::string(PP_BROWSER_ASSETS_DIR) + "/fonts/LatoLatin-Regular.ttf"));
    context = ui::CreateContext("wide-stylesheet-test", {1280, 800});
    ASSERT_NE(context, nullptr);
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("wide-stylesheet-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  /** "id left top width height display font-size" for every scene element, laid out with the real sheets. */
  std::string Measure(const std::string& root_class, int width_dp, bool with_wide) {
    const std::string rml = "<rml><head><style>" + Asset("themes/foundation.rcss") + Asset("themes/components.rcss") +
                            (with_wide ? Asset("themes/wide.rcss") : std::string()) +
                            Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">" +
                            Scene(root_class, width_dp) + "</body></rml>";
    ui::ElementDocument* document = context->LoadDocumentFromMemory(rml);
    EXPECT_NE(document, nullptr);
    if (!document) {
      return {};
    }
    document->Show();
    context->Update();
    context->Update();
    std::ostringstream out;
    for (const char* id : kIds) {
      ui::Element* element = document->GetElementById(id);
      if (!element) {
        out << id << " missing\n";
        continue;
      }
      const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
      const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
      out << id << ' ' << at.x << ' ' << at.y << ' ' << size.x << ' ' << size.y;
      for (const char* property :
           {"display", "font-size", "padding-left", "border-top-left-radius", "min-height", "min-width", "gap"}) {
        const ui::Property* value = element->GetProperty(property);
        out << ' ' << (value ? value->ToString() : ui::String("-"));
      }
      out << '\n';
    }
    measured_width_ = {};
    for (const char* id : {"rail", "list", "conv"}) {
      measured_width_.push_back(document->GetElementById(id)->GetBox().GetSize(ui::BoxArea::Border).x);
    }
    document->Close();
    context->Update();
    return out.str();
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  std::vector<float> measured_width_;
};

TEST(WideStylesheetSelectorsTest, EverySelectorIsRootedAtTheWideRoot) {
  std::string css = Asset("themes/wide.rcss");
  ASSERT_FALSE(css.empty());
  css = std::regex_replace(css, std::regex("/\\*[\\s\\S]*?\\*/"), "");
  int rules = 0;
  std::regex rule("([^{}]+)\\{([^{}]*)\\}");
  for (std::sregex_iterator it(css.begin(), css.end(), rule), end; it != end; ++it) {
    std::stringstream selectors((*it)[1].str());
    std::string one;
    while (std::getline(selectors, one, ',')) {
      const size_t first = one.find_first_not_of(" \n\t");
      ASSERT_NE(first, std::string::npos);
      EXPECT_EQ(one.compare(first, 17, ".shell-body--wide"), 0) << "not wide-rooted: " << one;
      ++rules;
    }
    EXPECT_EQ((*it)[2].str().find('#'), std::string::npos) << "a colour in wide.rcss: " << (*it)[1].str();
  }
  EXPECT_GT(rules, 20);
}

TEST_F(WideStylesheetTest, UnderThePhonesRootTheWideSheetChangesNothing) {
  for (const int width : {375, 393}) {
    const std::string without = Measure("shell-body--compact", width, /*with_wide=*/false);
    const std::string with = Measure("shell-body--compact", width, /*with_wide=*/true);
    ASSERT_FALSE(without.empty());
    EXPECT_EQ(with, without) << "the wide sheet leaked into the compact root at " << width;
  }
}

TEST_F(WideStylesheetTest, UnderTheWideRootTheSheetDoesApply) {
  // The control for the test above: the same scene with the wide root does differ.
  const std::string compact = Measure("shell-body--compact", 1280, /*with_wide=*/true);
  const std::string wide = Measure("shell-body--wide", 1280, /*with_wide=*/true);
  EXPECT_NE(wide, compact);
  EXPECT_FLOAT_EQ(measured_width_[0], 64.f) << "the icon rail is 64dp wide";
}

TEST_F(WideStylesheetTest, TheColumnsGiveWayAt768And900And1280) {
  for (const int width : {768, 900, 1280}) {
    Measure("shell-body--wide", width, /*with_wide=*/true);
    const float rail = measured_width_[0];
    const float list = measured_width_[1];
    const float conv = measured_width_[2];
    EXPECT_FLOAT_EQ(rail, 64.f) << width;
    EXPECT_GE(list, 280.f) << "list floor at " << width;
    EXPECT_GE(conv, 420.f) << "conversation floor at " << width;
    EXPECT_NEAR(rail + list + conv, static_cast<float>(width), 1.f) << "the three columns fill " << width;
    EXPECT_LE(list, 320.f) << width;
  }
  Measure("shell-body--wide", 900, true);
  EXPECT_FLOAT_EQ(measured_width_[1], 320.f) << "at 900 the list keeps its default 320";
  Measure("shell-body--wide", 768, true);
  EXPECT_LT(measured_width_[1], 320.f) << "at 768 the list is the one that gives way";
}

} // namespace
} // namespace pbr
