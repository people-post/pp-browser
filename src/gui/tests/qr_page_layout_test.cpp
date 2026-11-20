// The QR sheet on the real stylesheets at a tall, a small and a short-wide phone page: the code is never larger than
// the page leaves room for, the name and buttons stay on screen (or the body scrolls), and the buttons are as wide as
// the code.
#include "gui/QrDisplay.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
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

std::string Asset(const char* relative) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

class QrPageLayoutTest : public testing::Test {
protected:
  void Mount(const int width, const int height) {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("qr-page-layout-test", {width, height});
    ASSERT_NE(context, nullptr);
    display = QrDisplayFor(/*compact=*/true, static_cast<float>(width), static_cast<float>(height), 1.f);
    // The markup of my_qr.rml with the data bindings written out: the code and the buttons take the bound size.
    const std::string side = display.css;
    const std::string rml =
        "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0;\"><div style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(width) + "px; height: " + std::to_string(height) +
        "px;\"><div class=\"shell-layer shell-layer-overlay shell-layer-compact\">"
        "<div class=\"shell-page-sheet\"><div class=\"qr-page\"><div class=\"sheet-header\"><p "
        "class=\"sheet-title\">My QR "
        "code</p></div><div class=\"qr-body\" id=\"qr-body\"><div class=\"qr-card\"><div class=\"qr-code\" id=\"code\" "
        "style=\"width: " +
        side + "; height: " + side +
        ";\"></div></div><p class=\"qr-name\">Kenneth</p><p class=\"qr-hint\">Ask a friend to scan this to add "
        "you.</p><div class=\"qr-actions\" id=\"actions\" style=\"width: " +
        side +
        ";\"><button class=\"btn btn-primary qr-action\" type=\"button\" id=\"share\">Share QR code</button><button "
        "class=\"btn btn-secondary qr-action\" type=\"button\" id=\"save\">Save to Photos</button></div></div></div>"
        "</div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("qr-page-layout-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  float Width(const char* id) { return document->GetElementById(id)->GetBox().GetSize(ui::BoxArea::Border).x; }
  float Bottom(const char* id) {
    ui::Element* e = document->GetElementById(id);
    return e->GetAbsoluteOffset(ui::BoxArea::Border).y + e->GetBox().GetSize(ui::BoxArea::Border).y;
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  QrDisplay display;
};

TEST_F(QrPageLayoutTest, ATallPageShowsTheFullCodeAndEverythingUnderIt) {
  Mount(393, 852);
  EXPECT_EQ(display.side_dp, 340);
  EXPECT_FLOAT_EQ(Width("code"), 340.f);
  EXPECT_FLOAT_EQ(Width("actions"), Width("code")) << "the buttons are as wide as the code";
  EXPECT_FLOAT_EQ(Width("share"), Width("code"));
  EXPECT_LE(Bottom("save"), 852.f);
}

TEST_F(QrPageLayoutTest, ASmallPhoneShrinksTheCodeSoTheButtonsStayOnScreen) {
  Mount(375, 667);
  EXPECT_LT(display.side_dp, 335) << "the height, not the width, limits the code here";
  EXPECT_FLOAT_EQ(Width("code"), static_cast<float>(display.side_dp));
  EXPECT_FLOAT_EQ(Width("actions"), Width("code"));
  EXPECT_LE(Bottom("save"), 667.f);
}

TEST_F(QrPageLayoutTest, APhoneOnItsSideKeepsASmallCodeAndTheBodyScrollsToTheButtons) {
  Mount(852, 393);
  EXPECT_EQ(display.side_dp, 200);
  EXPECT_FLOAT_EQ(Width("code"), 200.f);
  ui::Element* body = document->GetElementById("qr-body");
  EXPECT_GT(body->GetScrollHeight(), body->GetClientHeight()) << "more than fits: it scrolls";
  EXPECT_EQ(body->GetComputedValues().overflow_y(), ui::Style::Overflow::Auto);
}

} // namespace
} // namespace pbr
