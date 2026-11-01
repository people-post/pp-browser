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

} // namespace
} // namespace pbr
