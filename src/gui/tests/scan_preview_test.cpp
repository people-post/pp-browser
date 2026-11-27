#include "gui/scan/ElementScanPreview.h"
#include "gui/scan/QrScanController.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

namespace pbr {
namespace {

class PreviewRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex> vertices, ui::Span<const int>) override {
    // The picture's quad is the first four-vertex mesh of a frame (the scan line's quad comes after it).
    if (vertices.size() == 4 && picture_left_u < 0.f) {
      picture_left_u = vertices[0].tex_coord.x;
      picture_right_u = vertices[1].tex_coord.x;
    }
    return ++geometry_id;
  }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle texture) override {
    if (texture) {
      ++textured_draws;
    }
  }
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte> pixels, ui::Vector2i size) override {
    EXPECT_EQ(size, ui::Vector2i(4, 2));
    EXPECT_EQ(pixels.size(), 32u);
    return ++generated;
  }
  void ReleaseTexture(ui::TextureHandle) override { ++released; }
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
  uintptr_t geometry_id = 0;
  uintptr_t generated = 0;
  uintptr_t released = 0;
  int textured_draws = 0;
  float picture_left_u = -1.f;
  float picture_right_u = -1.f;
};

class ScanPreviewTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    RegisterScanPreviewElement();
    context = ui::CreateContext("scan-preview-test", {400, 400});
    ASSERT_NE(context, nullptr);
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("scan-preview-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  PreviewRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
};

TEST_F(ScanPreviewTest, ActualScanViewMountsAndPreviewTextureDiesWithTheDocument) {
  MediaDeviceArbiter devices(CreateNullMediaDeviceBackend());
  QrScanController scan(devices);
  ASSERT_TRUE(scan.RegisterModel(context));
  std::ifstream file(std::string(PP_BROWSER_ASSETS_DIR) + "/views/qr_scan.rml");
  ASSERT_TRUE(file.good());
  std::ostringstream body;
  body << file.rdbuf();
  const auto localized = LocalizationService::Instance().LocalizeText(body.str());
  const std::string rml =
      "<rml><head><style>scan-preview {display:block; width:200px; height:200px;}"
      "</style></head><body>" +
      localized + "</body></rml>";
  auto* document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  auto* preview = dynamic_cast<ElementScanPreview*>(document->GetElementById("qr-scan-preview"));
  ASSERT_NE(preview, nullptr);
  preview->SetFrame(4, 2, std::vector<uint8_t>(32, 255));
  document->Show();
  context->Update();
  context->Render();
  EXPECT_EQ(renderer.generated, 1u);
  EXPECT_GT(renderer.textured_draws, 0);
  document->Close();
  context->Update();
  EXPECT_EQ(renderer.released, renderer.generated);
}

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// Phones (Kenneth 2026-11-04, WeChat style): inside the shell's full-screen layer the camera and the finder fill the
// layer, the 250dp frame sits in its middle, the back and "My QR code" buttons are there and the desktop title,
// status line and buttons are not. "My QR code" leaves the scan without a result and shows my code.
TEST_F(ScanPreviewTest, ThePhoneScanFillsTheScreenAndMyQrLeavesWithoutAResult) {
  MediaDeviceArbiter devices(CreateNullMediaDeviceBackend());
  QrScanController scan(devices);
  bool full_screen = false;
  int closed = 0;
  ShellNavigationPorts navigation;
  navigation.push_layer = [&](const PaneSpec& spec) {
    full_screen = spec.full_screen;
    return 3;
  };
  navigation.close_layer = [&](int) { ++closed; };
  scan.BindShellNavigation(std::move(navigation));
  int my_qr = 0;
  scan.BindShowMyQr([&]() { ++my_qr; });
  ASSERT_TRUE(scan.RegisterModel(context));
  int delivered = 0;
  scan.Open([&](std::string) { ++delivered; });
  EXPECT_TRUE(full_screen);

  const std::string view =
      LocalizationService::Instance().LocalizeText(ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/views/qr_scan.rml"));
  const std::string rml = "<rml><head><style>" +
                          ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss") +
                          "body { width: 400px; height: 400px; }</style></head><body>"
                          "<div class=\"shell-layer shell-layer-overlay shell-layer-full-screen\">"
                          "<div class=\"shell-overlay-body shell-layer-compact\">" +
                          view + "</div></div></body></rml>";
  auto* document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  auto first = [&](const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  };
  for (const char* shown :
       {"qr-scan-camera", "qr-scan-finder", "qr-scan-frame", "qr-scan-line", "qr-scan-back", "qr-scan-tool"}) {
    ASSERT_NE(first(shown), nullptr) << shown;
    EXPECT_TRUE(first(shown)->IsVisible(true)) << shown;
  }
  for (const char* hidden : {"qr-scan-title", "qr-scan-status", "qr-scan-actions"}) {
    ASSERT_NE(first(hidden), nullptr) << hidden;
    EXPECT_FALSE(first(hidden)->IsVisible(true)) << hidden;
  }
  // The camera covers the whole 400x400 layer; the frame is a 250 square in its middle.
  const ui::Vector2f camera = first("qr-scan-camera")->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_FLOAT_EQ(camera.x, 400.f);
  EXPECT_FLOAT_EQ(camera.y, 400.f);
  ui::Element* frame = first("qr-scan-frame");
  EXPECT_FLOAT_EQ(frame->GetBox().GetSize(ui::BoxArea::Border).x, 250.f);
  EXPECT_FLOAT_EQ(frame->GetBox().GetSize(ui::BoxArea::Border).y, 250.f);
  EXPECT_FLOAT_EQ(frame->GetAbsoluteOffset(ui::BoxArea::Border).x, 75.f);
  EXPECT_FLOAT_EQ(frame->GetAbsoluteOffset(ui::BoxArea::Border).y, 75.f);
  // The hint sits under the frame.
  ASSERT_NE(first("qr-scan-hint"), nullptr);
  EXPECT_GE(first("qr-scan-hint")->GetAbsoluteOffset(ui::BoxArea::Border).y, 75.f + 250.f);
  // Both buttons are at least 44 tall.
  EXPECT_GE(first("qr-scan-back")->GetBox().GetSize(ui::BoxArea::Border).y, 44.f);
  EXPECT_GE(first("qr-scan-tool")->GetBox().GetSize(ui::BoxArea::Border).y, 44.f);

  first("qr-scan-tool")->Click();
  context->Update();
  EXPECT_EQ(my_qr, 1);
  EXPECT_EQ(closed, 1);
  EXPECT_FALSE(scan.IsOpen());
  EXPECT_EQ(delivered, 0);
  document->Close();
  context->Update();
}

// The scan layer on a desktop computer (the same full-screen layer, its body .shell-layer-computer): the camera fills
// the window, the picture is the whole frame aspect-fit with the guide on it, the back is at the top left and the hint
// and Cancel sit at the bottom -- all of it inside the window, at every desktop size. The phone's layer is the test above.
TEST_F(ScanPreviewTest, TheComputerScanFillsTheWindowAndKeepsItsControlsOnScreen) {
  MediaDeviceArbiter devices(CreateNullMediaDeviceBackend());
  QrScanController scan(devices);
  ASSERT_TRUE(scan.RegisterModel(context));
  const std::string view =
      LocalizationService::Instance().LocalizeText(ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/views/qr_scan.rml"));
  struct Window {
    int width, height;
  };
  for (const Window window : {Window{1280, 800}, Window{1100, 700}, Window{1024, 640}, Window{900, 600},
                              Window{800, 560}, Window{500, 800}}) {
    const std::string where = std::to_string(window.width) + "x" + std::to_string(window.height);
    context->SetDimensions({window.width, window.height});
    const std::string rml = "<rml><head><style>" +
                            ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss") + "body { width: " +
                            std::to_string(window.width) + "px; height: " + std::to_string(window.height) +
                            "px; }</style></head><body>"
                            "<div class=\"shell-layer shell-layer-overlay shell-layer-full-screen\">"
                            "<div class=\"shell-overlay-body shell-layer-computer\">" +
                            view + "</div></div></body></rml>";
    auto* document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr) << where;
    document->Show();
    context->Update();
    auto first = [&](const char* cls) {
      ui::ElementList found;
      document->GetElementsByClassName(found, cls);
      return found.empty() ? nullptr : found.front();
    };
    for (const char* shown : {"qr-scan-camera", "qr-scan-back", "qr-scan-computer-bar", "qr-scan-computer-hint",
                              "qr-scan-actions"}) {
      ASSERT_NE(first(shown), nullptr) << where << " " << shown;
      EXPECT_TRUE(first(shown)->IsVisible(true)) << where << " " << shown;
    }
    for (const char* hidden : {"qr-scan-finder", "qr-scan-title", "qr-scan-status", "qr-scan-tools"}) {
      ASSERT_NE(first(hidden), nullptr) << where << " " << hidden;
      EXPECT_FALSE(first(hidden)->IsVisible(true)) << where << " " << hidden;
    }
    const float w = static_cast<float>(window.width);
    const float h = static_cast<float>(window.height);
    // The camera and the preview cover the whole window.
    for (const char* filled : {"qr-scan-camera", "qr-scan-preview"}) {
      const ui::Vector2f size = first(filled)->GetBox().GetSize(ui::BoxArea::Border);
      EXPECT_FLOAT_EQ(size.x, w) << where << " " << filled;
      EXPECT_FLOAT_EQ(size.y, h) << where << " " << filled;
    }
    // Back, hint and the buttons are inside the window; the targets are at least 44 tall.
    for (const char* inside : {"qr-scan-back", "qr-scan-computer-bar", "qr-scan-actions"}) {
      const ui::Vector2f at = first(inside)->GetAbsoluteOffset(ui::BoxArea::Border);
      const ui::Vector2f size = first(inside)->GetBox().GetSize(ui::BoxArea::Border);
      EXPECT_GE(at.x, -0.5f) << where << " " << inside;
      EXPECT_GE(at.y, -0.5f) << where << " " << inside;
      EXPECT_LE(at.x + size.x, w + 0.5f) << where << " " << inside;
      EXPECT_LE(at.y + size.y, h + 0.5f) << where << " " << inside;
    }
    EXPECT_GE(first("qr-scan-back")->GetBox().GetSize(ui::BoxArea::Border).y, 44.f) << where;
    ui::ElementList buttons;
    first("qr-scan-actions")->GetElementsByTagName(buttons, "button");
    int shown_buttons = 0;
    for (ui::Element* button : buttons) {
      if (button->IsVisible(true)) {
        ++shown_buttons;
        EXPECT_GE(button->GetBox().GetSize(ui::BoxArea::Border).y, 43.9f) << where;
        EXPECT_LE(button->GetAbsoluteOffset(ui::BoxArea::Border).y + button->GetBox().GetSize(ui::BoxArea::Border).y,
                  h + 0.5f)
            << where;
      }
    }
    EXPECT_EQ(shown_buttons, 1) << where << ": Cancel (Try again only after a failure)";
    // The picture: a 1920x1080 frame whole, aspect-fit, with the 70% guide on it, clear of the bottom bar.
    const ui::Vector2f box = first("qr-scan-preview")->GetBox().GetSize(ui::BoxArea::Content);
    const ScanPreviewLayout layout = LayoutScanPreview(box, 1920, 1080, true);
    EXPECT_NEAR(layout.image_size.x / layout.image_size.y, 1920.f / 1080.f, 0.001f) << where;
    EXPECT_GE(layout.image_offset.x, -0.01f) << where;
    EXPECT_GE(layout.image_offset.y, -0.01f) << where;
    EXPECT_LE(layout.image_offset.x + layout.image_size.x, box.x + 0.01f) << where;
    EXPECT_LE(layout.image_offset.y + layout.image_size.y, box.y + 0.01f) << where;
    EXPECT_NEAR(layout.guide_side, 0.7f * std::min(layout.image_size.x, layout.image_size.y), 0.01f) << where;
    const float guide_bottom = layout.image_offset.y + (layout.image_size.y + layout.guide_side) / 2.f;
    EXPECT_LE(layout.guide_side, std::min(layout.image_size.x, layout.image_size.y)) << where << ": guide inside the picture";
    EXPECT_LE(guide_bottom, first("qr-scan-computer-bar")->GetAbsoluteOffset(ui::BoxArea::Border).y + 0.5f)
        << where << ": the bar does not cover the guide";
    // A portrait frame fits the other way round.
    const ScanPreviewLayout tall = LayoutScanPreview(box, 1080, 1920, true);
    EXPECT_LE(tall.image_size.y, box.y + 0.01f) << where;
    EXPECT_LE(tall.image_size.x, box.x + 0.01f) << where;
    document->Close();
    context->Update();
  }
  context->SetDimensions({400, 400});
}

// The mirror is drawn, not baked in: the quad's texture coordinates run right to left in a computer's layer when the
// camera asks for it, and never on a phone's (or the framed panel's), whatever the flag says.
TEST_F(ScanPreviewTest, OnlyAComputersLayerDrawsThePreviewMirrored) {
  for (const char* layer : {"shell-layer-computer", "shell-layer-compact"}) {
    for (const bool mirrored : {false, true}) {
      const std::string rml = std::string("<rml><head><style>scan-preview {display:block; width:200px; height:200px;}"
                                          "</style></head><body><div class=\"") +
                              layer + "\"><scan-preview id=\"p\"></scan-preview></div></body></rml>";
      renderer.picture_left_u = renderer.picture_right_u = -1.f;
      auto* document = context->LoadDocumentFromMemory(rml);
      ASSERT_NE(document, nullptr);
      auto* preview = dynamic_cast<ElementScanPreview*>(document->GetElementById("p"));
      ASSERT_NE(preview, nullptr);
      preview->SetMirrored(mirrored);
      preview->SetFrame(4, 2, std::vector<uint8_t>(32, 255));
      document->Show();
      context->Update();
      context->Render();
      const bool flipped = mirrored && std::string(layer) == "shell-layer-computer";
      EXPECT_EQ(renderer.picture_left_u, flipped ? 1.f : 0.f) << layer << " mirrored=" << mirrored;
      EXPECT_EQ(renderer.picture_right_u, flipped ? 0.f : 1.f) << layer << " mirrored=" << mirrored;
      document->Close();
      context->Update();
    }
  }
}

TEST(ScanPreviewLayoutTest, ThePhoneAndThePanelKeepTheCoveringPicture) {
  // Cover: the picture is larger than the box on one axis, the guide the old 68% of the box.
  const ScanPreviewLayout layout = LayoutScanPreview({400.f, 400.f}, 1920, 1080, false);
  EXPECT_GE(layout.image_size.x, 400.f);
  EXPECT_FLOAT_EQ(layout.image_size.y, 400.f);
  EXPECT_NEAR(layout.guide_side, 272.f, 0.01f);
}

} // namespace
} // namespace pbr
