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
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override {
    return ++geometry_id;
  }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle texture) override {
    if (texture) { ++textured_draws; }
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
    if (context) { ui::RemoveContext("scan-preview-test"); }
    if (initialized) { ui::Shutdown(); }
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
  const std::string rml = "<rml><head><style>scan-preview {display:block; width:200px; height:200px;}"
                          "</style></head><body>" + localized + "</body></rml>";
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

  const std::string view = LocalizationService::Instance().LocalizeText(
      ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/views/qr_scan.rml"));
  const std::string rml = "<rml><head><style>" + ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss") +
                          "body { width: 400px; height: 400px; }</style></head><body>"
                          "<div class=\"shell-layer shell-layer-overlay shell-layer-full-screen\">"
                          "<div class=\"shell-overlay-body shell-layer-compact\">" + view + "</div></div></body></rml>";
  auto* document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  auto first = [&](const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  };
  for (const char* shown : {"qr-scan-camera", "qr-scan-finder", "qr-scan-frame", "qr-scan-line", "qr-scan-back",
                            "qr-scan-tool"}) {
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

} // namespace
} // namespace pbr
