// The avatar crop page (PLAN-avatar-edit items 9, 11, 12): its markup and registration, the custom element's clipping,
// the wheel and the viewport arithmetic, the lock on Done, one page at a time.
#include "foundation/i18n/LocalizationService.h"
#include "gui/avatar/AvatarCropController.h"
#include "gui/avatar/ElementAvatarCrop.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/ViewCatalog.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>

namespace pbr {
namespace {

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string ReadView() {
  return ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/views/avatar_crop.rml");
}

TEST(AvatarCropMarkupTest, ThePageHasTheTitleTheSurfaceAndThreeButtonsWithTheirEvents) {
  const std::string rml = ReadView();
  ASSERT_FALSE(rml.empty());
  EXPECT_NE(rml.find("data-model=\"avatar_crop\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.profile.crop_title}}"), std::string::npos);
  EXPECT_NE(rml.find("<avatar-crop id=\"avatar-crop-surface\""), std::string::npos);
  // Cancel left, Rotate in the middle, Done right (the order they appear in).
  const size_t cancel = rml.find("data-event-click=\"crop_cancel()\"");
  const size_t rotate = rml.find("data-event-click=\"crop_rotate()\"");
  const size_t done = rml.find("data-event-click=\"crop_done()\"");
  ASSERT_NE(cancel, std::string::npos);
  ASSERT_NE(rotate, std::string::npos);
  ASSERT_NE(done, std::string::npos);
  EXPECT_LT(cancel, rotate);
  EXPECT_LT(rotate, done);
  EXPECT_NE(rml.find("{{i18n:common.cancel}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.profile.crop_rotate}}"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.profile.crop_done}}"), std::string::npos);
  // Done and Rotate are off while loading or locked; Cancel while locked.
  EXPECT_NE(rml.find("data-attrif-disabled=\"crop_locked || crop_loading\""), std::string::npos);
  EXPECT_NE(rml.find("data-attrif-disabled=\"crop_locked\""), std::string::npos);
}

TEST(AvatarCropMarkupTest, TheViewIsRegisteredInTheCatalog) {
  EXPECT_EQ(ViewCatalog::ResolvePath("avatar_crop"), "views/avatar_crop.rml");
  bool listed = false;
  for (const std::string& path : ViewCatalog::KnownViewPaths()) {
    listed = listed || path == "views/avatar_crop.rml";
  }
  EXPECT_TRUE(listed);
}

TEST(AvatarCropRulesTest, TheViewportCirclePicksTheSmallestOfWidthHeightAndTheCap) {
  EXPECT_DOUBLE_EQ(AvatarCropViewportDiameter(400, 800), 320.0);
  EXPECT_DOUBLE_EQ(AvatarCropViewportDiameter(300, 800), 252.0);
  EXPECT_DOUBLE_EQ(AvatarCropViewportDiameter(800, 200), 152.0);
  EXPECT_DOUBLE_EQ(AvatarCropViewportDiameter(10, 10), 1.0) << "not laid out yet";
}

TEST(AvatarCropRulesTest, TheWheelZoomsInWhenUpAndEachEventIsClamped) {
  EXPECT_GT(AvatarCropWheelFactor(-1.0), 1.0);
  EXPECT_LT(AvatarCropWheelFactor(1.0), 1.0);
  EXPECT_DOUBLE_EQ(AvatarCropWheelFactor(-100.0), 1.1);
  EXPECT_DOUBLE_EQ(AvatarCropWheelFactor(100.0), 0.9);
  EXPECT_DOUBLE_EQ(AvatarCropWheelFactor(0.0), 1.0);
  // A trackpad's small values are a small change, not a jump to the limit.
  EXPECT_NEAR(AvatarCropWheelFactor(-0.05), std::pow(1.0015, 4.0), 1e-9);
  EXPECT_DOUBLE_EQ(AvatarCropWheelFactor(std::nan("")), 1.0);
}

TEST(AvatarCropRulesTest, DoneAndRotateNeedAPhotoAndNothingInFlightAndCancelOnlyWhenNotLocked) {
  EXPECT_TRUE(AvatarCropButtons::CanDone(false, false, true));
  EXPECT_FALSE(AvatarCropButtons::CanDone(true, false, true)) << "still loading";
  EXPECT_FALSE(AvatarCropButtons::CanDone(false, true, true)) << "a second tap on Done";
  EXPECT_FALSE(AvatarCropButtons::CanDone(false, false, false)) << "no photo";
  EXPECT_FALSE(AvatarCropButtons::CanRotate(false, true, true));
  EXPECT_TRUE(AvatarCropButtons::CanCancel(false));
  EXPECT_FALSE(AvatarCropButtons::CanCancel(true));
}

// Records whether geometry with the photo's texture is drawn with a scissor region on.
class CropRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override {
    return ++geometry_id;
  }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle texture) override {
    if (texture) {
      ++textured_draws;
      if (scissor) {
        ++textured_draws_clipped;
      }
      last_scissor = region;
    }
  }
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return ++generated; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool enable) override { scissor = enable; }
  void SetScissorRegion(ui::Rectanglei r) override { region = r; }
  uintptr_t geometry_id = 0;
  uintptr_t generated = 0;
  int textured_draws = 0;
  int textured_draws_clipped = 0;
  bool scissor = false;
  ui::Rectanglei region;
  ui::Rectanglei last_scissor;
};

class AvatarCropPageTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    RegisterAvatarCropElement();
    context = ui::CreateContext("avatar-crop-test", {400, 700});
    ASSERT_NE(context, nullptr);
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("avatar-crop-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  CropRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
};

// An `overflow: hidden` stage does not clip a custom element's geometry on its own (the library clips by what the
// ancestor's content overflows), so the element sets its own clip: the textured quad is drawn with a scissor region,
// and the region is the element's box.
TEST_F(AvatarCropPageTest, ThePhotoIsDrawnClippedToTheElementsBox) {
  AvatarCropController page;
  ASSERT_TRUE(page.RegisterModel(context));
  const std::string view = LocalizationService::Instance().LocalizeText(ReadView());
  const std::string rml = "<rml><head><style>" +
                          ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss") +
                          "body { width: 400px; height: 700px; }</style></head><body>"
                          "<div class=\"shell-layer shell-layer-overlay shell-layer-full-screen\">"
                          "<div class=\"shell-overlay-body shell-layer-compact\">" +
                          view + "</div></div></body></rml>";
  auto* document = context->LoadDocumentFromMemory(rml);
  ASSERT_NE(document, nullptr);
  auto* surface = dynamic_cast<ElementAvatarCrop*>(document->GetElementById("avatar-crop-surface"));
  ASSERT_NE(surface, nullptr);
  document->Show();
  context->Update();

  surface->SetImage(4, 2, std::vector<uint8_t>(32, 255));
  AvatarCropFrame frame(4, 2, 200);
  frame.Pinch({0, 0}, 3.0, 0.5235987755982988); // a big, turned picture: far past the box
  surface->SetView(frame.Quad(), frame.viewport_diameter());
  context->Update();
  context->Render();
  ASSERT_GT(renderer.textured_draws, 0);
  EXPECT_EQ(renderer.textured_draws_clipped, renderer.textured_draws);
  const ui::Vector2f box = surface->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_GT(box.x, 0.f);
  EXPECT_EQ(renderer.last_scissor.Width(), static_cast<int>(box.x));
  EXPECT_EQ(renderer.last_scissor.Height(), static_cast<int>(box.y));

  // The page's own controls are the sizes a thumb needs.
  for (const char* cls : {"avatar-crop-btn--cancel", "avatar-crop-btn--rotate", "avatar-crop-btn--done"}) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    ASSERT_FALSE(found.empty()) << cls;
    EXPECT_GE(found.front()->GetBox().GetSize(ui::BoxArea::Border).y, 44.f) << cls;
    EXPECT_GE(found.front()->GetBox().GetSize(ui::BoxArea::Border).x, 44.f) << cls;
  }
  document->Close();
  context->Update();
}

} // namespace
} // namespace pbr
