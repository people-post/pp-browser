// A link in a message bubble on the real stylesheets, both themes: in our own (blue) bubble it takes the bubble's white and
// stays underlined (blue on blue was unreadable); in a received bubble it keeps the link colour.
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <ui/style/ComputedValues.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
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

std::string AssetPath(const std::string& relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

struct Rgb {
  int r, g, b;
};

// Link and white tokens as theme_tokens.tsv spells them (kept here so a palette change is a deliberate test edit).
struct Theme {
  const char* name;
  Rgb link;
};

class ChatLinkColourTest : public testing::TestWithParam<Theme> {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("chat-link-colour-test", {390, 844});
    ASSERT_NE(context, nullptr);
    const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
                            ReadAll(AssetPath(std::string("themes/colors-") + GetParam().name + ".rcss")) +
                            "</style></head><body style=\"font-family: Inter; font-size: 15dp;\">"
                            "<div class=\"bubble bubble-user\"><p class=\"bubble-text\">see <span class=\"chat-link\" "
                            "id=\"own\">pp.example</span></p></div>"
                            "<div class=\"bubble bubble-assistant\"><p class=\"bubble-text-peer\">see <span "
                            "class=\"chat-link\" id=\"in\">pp.example</span></p></div>"
                            "</body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->ActivateTheme(GetParam().name, true);
    context->Update();
    context->Update();
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("chat-link-colour-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
};

TEST_P(ChatLinkColourTest, ALinkInOurOwnBubbleIsWhiteAndUnderlined) {
  ui::Element* link = document->GetElementById("own");
  ASSERT_NE(link, nullptr);
  const ui::Colourb colour = link->GetComputedValues().color();
  EXPECT_EQ(colour.red, 255);
  EXPECT_EQ(colour.green, 255);
  EXPECT_EQ(colour.blue, 255);
  EXPECT_EQ(colour.alpha, 255);
  EXPECT_EQ(link->GetComputedValues().text_decoration(), ui::Style::TextDecoration::Underline);
}

TEST_P(ChatLinkColourTest, ALinkInAReceivedBubbleKeepsTheLinkColour) {
  ui::Element* link = document->GetElementById("in");
  ASSERT_NE(link, nullptr);
  const ui::Colourb colour = link->GetComputedValues().color();
  EXPECT_EQ(colour.red, GetParam().link.r);
  EXPECT_EQ(colour.green, GetParam().link.g);
  EXPECT_EQ(colour.blue, GetParam().link.b);
  EXPECT_EQ(link->GetComputedValues().text_decoration(), ui::Style::TextDecoration::Underline);
}

INSTANTIATE_TEST_SUITE_P(Themes, ChatLinkColourTest,
                         testing::Values(Theme{"light", {0x00, 0x7a, 0xff}}, Theme{"dark", {0x0a, 0x84, 0xff}}));

} // namespace
} // namespace pbr
