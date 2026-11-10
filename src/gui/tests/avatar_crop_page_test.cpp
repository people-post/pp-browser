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

TEST(AvatarCropMarkupTest, OnlyThePhoneHidesRotateAndShowsTheRoundCloseAndTheBarsSitAboveTheBlackBehindThem) {
  const std::string css = ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss");
  auto rule = [&](const std::string& selector) {
    const size_t at = css.find("\n" + selector + " {");
    EXPECT_NE(at, std::string::npos) << selector;
    return at == std::string::npos ? std::string() : css.substr(at, css.find('}', at) - at);
  };
  EXPECT_NE(rule(".shell-layer-compact .avatar-crop-btn--rotate").find("display: none"), std::string::npos);
  EXPECT_EQ(css.find("\n.avatar-crop-btn--rotate {"), std::string::npos) << "desktop keeps it";
  EXPECT_NE(rule(".avatar-crop-close").find("display: none"), std::string::npos);
  EXPECT_NE(rule(".shell-layer-compact .avatar-crop-close").find("display: flex"), std::string::npos);
  EXPECT_NE(rule(".shell-layer-compact .avatar-crop-actions").find("space-between"), std::string::npos);
  // The phone's black is absolute; everything the user presses must be positioned above it (z-index), or it is hidden.
  const size_t at = css.find(".avatar-crop-bar,\n.avatar-crop-stage,\n.avatar-crop-actions {");
  ASSERT_NE(at, std::string::npos);
  const std::string above = css.substr(at, css.find('}', at) - at);
  EXPECT_NE(above.find("position: relative"), std::string::npos);
  EXPECT_NE(above.find("z-index: 1"), std::string::npos);
  EXPECT_NE(rule(".avatar-view-stage").find("z-index: 1"), std::string::npos);
  const std::string rml = ReadView();
  EXPECT_NE(rml.find("class=\"avatar-crop-close shell-circle-btn\""), std::string::npos);
  // Pressed and disabled looks: a bound class (the button attribute alone showed nothing on the device), a pressed
  // state, and the pill that floats above the stage and lets taps through.
  EXPECT_NE(rml.find("data-class-avatar-crop-btn--disabled=\"crop_locked || crop_loading\""), std::string::npos);
  EXPECT_NE(rml.find("data-class-avatar-crop-btn--disabled=\"crop_locked\""), std::string::npos);
  EXPECT_NE(rml.find("data-class-avatar-crop-close--disabled=\"crop_locked\""), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"crop_uploading\""), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:settings.profile.photo_uploading}}"), std::string::npos);
  EXPECT_NE(rule(".avatar-crop-btn:active,\n.avatar-crop-close:active").find("opacity"), std::string::npos);
  const std::string host = rule(".avatar-crop-pill-host");
  EXPECT_NE(host.find("position: absolute"), std::string::npos);
  EXPECT_NE(host.find("z-index: 3"), std::string::npos);
  EXPECT_NE(host.find("pointer-events: none"), std::string::npos);
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
  // (Rotate is hidden in the phone layer this test mounts: two fingers turn the photo there.)
  for (const char* cls : {"avatar-crop-btn--cancel", "avatar-crop-btn--done"}) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    ASSERT_FALSE(found.empty()) << cls;
    EXPECT_GE(found.front()->GetBox().GetSize(ui::BoxArea::Border).y, 44.f) << cls;
    EXPECT_GE(found.front()->GetBox().GetSize(ui::BoxArea::Border).x, 44.f) << cls;
  }
  document->Close();
  context->Update();
}

// The shell's phone full-screen layer as ShellHost builds it (ShellLayoutRml::Overlays + ApplySafeAreaLayout): #shell-root
// starts below the status bar, the layer is lifted by that inset, its body padded by the top and bottom insets, and the
// view is mounted in the body. A 390 x 844 phone with a 47dp top and 34dp bottom inset.
constexpr float kTop = 47.f;
constexpr float kBottom = 34.f;

class AvatarCropLayoutTest : public AvatarCropPageTest {
protected:
  void SetUp() override {
    AvatarCropPageTest::SetUp();
    ui::RemoveContext("avatar-crop-test");
    context = ui::CreateContext("avatar-crop-test", {390, 844});
    ASSERT_NE(context, nullptr);
  }

  ui::ElementDocument* Mount(const std::string& view_file, const bool compact) {
    const std::string view = LocalizationService::Instance().LocalizeText(
        ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/views/" + view_file));
    const std::string insets = compact ? "style=\"padding-top:47dp; padding-bottom:34dp\"" : "";
    const std::string rml =
        "<rml><head><style>" + ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss") +
        "body { width: 390px; height: 844px; }</style></head><body>"
        "<div id=\"shell-root\" style=\"position:absolute; top:47dp; left:0; right:0; bottom:0;\">"
        "<div id=\"layer\" class=\"shell-layer shell-layer-overlay shell-layer-full-screen\" style=\"top:-47dp\">"
        "<div id=\"body\" class=\"shell-overlay-body" +
        std::string(compact ? " shell-layer-compact" : "") + "\" " + insets + ">" + view +
        "</div></div></div></body></rml>";
    ui::ElementDocument* document = context->LoadDocumentFromMemory(rml);
    EXPECT_NE(document, nullptr);
    if (document) {
      document->Show();
      context->Update();
    }
    return document;
  }

  static ui::Element* First(ui::ElementDocument* document, const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  }
  static float Top(ui::Element* e) { return e->GetAbsoluteOffset(ui::BoxArea::Border).y; }
  static float Bottom(ui::Element* e) { return Top(e) + e->GetBox().GetSize(ui::BoxArea::Border).y; }
  static float H(ui::Element* e) { return e->GetBox().GetSize(ui::BoxArea::Border).y; }
  static float W(ui::Element* e) { return e->GetBox().GetSize(ui::BoxArea::Border).x; }

  // The element a tap at the middle of `e` lands on, and whether it is `e` or inside it.
  bool TapReaches(ui::Element* e) {
    const ui::Vector2f at = e->GetAbsoluteOffset(ui::BoxArea::Border) + e->GetBox().GetSize(ui::BoxArea::Border) * 0.5f;
    ui::Element* hit = context->GetElementAtPoint(at);
    for (; hit; hit = hit->GetParentNode()) {
      if (hit == e) {
        return true;
      }
    }
    return false;
  }
};

// Regression (device, iPhone 11): the black layer behind the page is absolute, and an in-flow block paints under (and
// loses taps to) a positioned element, so the whole bottom row and the title were hidden and Cancel / Done could not be
// pressed. Everything the user needs must be laid out inside the visible area AND be the thing a tap reaches.
TEST_F(AvatarCropLayoutTest, OnAPhoneTheBarsAndButtonsAreInsideTheSafeAreaAndReceiveTheTaps) {
  ui::ElementDocument* document = Mount("avatar_crop.rml", /*compact=*/true);
  ASSERT_NE(document, nullptr);
  ui::Element* bar = First(document, "avatar-crop-bar");
  ui::Element* stage = First(document, "avatar-crop-stage");
  ui::Element* actions = First(document, "avatar-crop-actions");
  ui::Element* cancel = First(document, "avatar-crop-btn--cancel");
  ui::Element* rotate = First(document, "avatar-crop-btn--rotate");
  ui::Element* done = First(document, "avatar-crop-btn--done");
  ui::Element* close = First(document, "avatar-crop-close");
  ui::Element* surface = document->GetElementById("avatar-crop-surface");
  for (ui::Element* e : {bar, stage, actions, cancel, rotate, done, close, surface}) {
    ASSERT_NE(e, nullptr);
  }
  // The top bar starts below the top inset; the actions row ends at the bottom inset, its buttons 8dp above it.
  EXPECT_FLOAT_EQ(Top(bar), kTop);
  EXPECT_FLOAT_EQ(Bottom(actions), 844.f - kBottom);
  for (ui::Element* e : {cancel, done}) {
    EXPECT_GE(H(e), 50.f);
    EXPECT_GE(Top(e), Bottom(stage));
    EXPECT_LE(Bottom(e), 844.f - kBottom - 8.f);
    EXPECT_TRUE(TapReaches(e));
  }
  // The stage is exactly the space between the bar and the actions row, and the element fills it.
  EXPECT_FLOAT_EQ(Top(stage), Bottom(bar));
  EXPECT_FLOAT_EQ(Bottom(stage), Top(actions));
  EXPECT_FLOAT_EQ(H(surface), H(stage));
  EXPECT_FLOAT_EQ(W(surface), 390.f);
  // Cancel is on the left and Done on the right; there is no Rotate on a phone (two fingers turn the photo).
  EXPECT_LT(cancel->GetAbsoluteOffset(ui::BoxArea::Border).x, 100.f);
  EXPECT_GT(done->GetAbsoluteOffset(ui::BoxArea::Border).x + W(done), 290.f);
  EXPECT_FALSE(rotate->IsVisible(true));
  // The second way out: a round close at the top left, at least 44dp, inside the top safe area, receiving taps.
  EXPECT_TRUE(close->IsVisible(true));
  EXPECT_GE(W(close), 44.f);
  EXPECT_GE(H(close), 44.f);
  EXPECT_GE(Top(close), kTop);
  EXPECT_LT(close->GetAbsoluteOffset(ui::BoxArea::Border).x, 40.f);
  EXPECT_TRUE(TapReaches(close));
  EXPECT_TRUE(TapReaches(First(document, "avatar-crop-title")));
  document->Close();
  context->Update();
}

// The page's bindings as the controller exposes them, driven by the test.
struct CropFlags {
  bool loading = false;
  bool locked = false;
  bool uploading = false;
};

bool RegisterCropFlags(ui::Context* context, CropFlags* flags) {
  return DataModelHost::Instance().Register(context, "avatar_crop", [flags](ui::DataModelConstructor& ctor) {
    ctor.Bind("crop_loading", &flags->loading);
    ctor.Bind("crop_locked", &flags->locked);
    ctor.Bind("crop_uploading", &flags->uploading);
    for (const char* name : {"crop_cancel", "crop_rotate", "crop_done"}) {
      ctor.BindEventCallback(name, [](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {});
    }
  });
}

// After Done: while the upload runs a pill with "Uploading..." covers the picture, painted and hit-tested above the
// stage's element, taps pass through it to the bottom buttons; buttons look disabled the moment the page is locked;
// when it ends the pill goes and the buttons are back.
TEST_F(AvatarCropLayoutTest, WhileUploadingAPillCoversThePictureAndTheLockedButtonsLookDisabled) {
  CropFlags flags;
  ASSERT_TRUE(RegisterCropFlags(context, &flags));
  ui::ElementDocument* document = Mount("avatar_crop.rml", /*compact=*/true);
  ASSERT_NE(document, nullptr);
  ui::Element* stage = First(document, "avatar-crop-stage");
  ui::Element* cancel = First(document, "avatar-crop-btn--cancel");
  ui::Element* done = First(document, "avatar-crop-btn--done");
  ui::Element* close = First(document, "avatar-crop-close");
  ASSERT_NE(stage, nullptr);
  auto pill_shown = [&]() {
    ui::Element* p = First(document, "avatar-crop-pill");
    return p && p->IsVisible(true);
  };
  EXPECT_FALSE(pill_shown()) << "no pill before Done";
  for (ui::Element* e : {cancel, done}) {
    EXPECT_FALSE(e->IsClassSet("avatar-crop-btn--disabled"));
    EXPECT_FALSE(e->HasAttribute("disabled"));
  }

  // Done accepted, the PIN prompt up: locked, no pill (the PIN pad is the feedback).
  flags.locked = true;
  DataModelHost::Instance().Dirty("avatar_crop", "crop_locked");
  context->Update();
  for (ui::Element* e : {cancel, done}) {
    EXPECT_TRUE(e->IsClassSet("avatar-crop-btn--disabled"));
    EXPECT_TRUE(e->HasAttribute("disabled"));
  }
  EXPECT_TRUE(close->IsClassSet("avatar-crop-close--disabled"));
  EXPECT_FALSE(pill_shown());

  // The upload runs: the pill is there, centred over the stage, and the topmost thing at the stage's middle.
  flags.uploading = true;
  DataModelHost::Instance().Dirty("avatar_crop", "crop_uploading");
  context->Update();
  ui::Element* pill = First(document, "avatar-crop-pill");
  ASSERT_NE(pill, nullptr);
  EXPECT_TRUE(pill->IsVisible(true));
  EXPECT_NEAR(Top(pill) + H(pill) / 2, Top(stage) + H(stage) / 2, 1.f);
  EXPECT_NEAR(pill->GetAbsoluteOffset(ui::BoxArea::Border).x + W(pill) / 2, 195.f, 1.f);
  EXPECT_FALSE(pill->GetInnerRML().empty()) << "the localised 'Uploading' text";
  EXPECT_EQ(pill->GetInnerRML().find("i18n"), std::string::npos);
  // Painted above the picture and the dim: a positioned host with a z-index inside the stage, where the element is
  // in the flow. Its taps pass through (pointer-events: none), so a tap on the pill reaches the picture element.
  ui::Element* host = First(document, "avatar-crop-pill-host");
  ASSERT_NE(host, nullptr);
  EXPECT_GT(host->GetZIndex(), 0.f);
  EXPECT_EQ(host->GetParentNode(), stage);
  EXPECT_FALSE(TapReaches(pill));
  EXPECT_TRUE(TapReaches(document->GetElementById("avatar-crop-surface")));
  // It does not block the bottom buttons: the host takes no taps, the buttons are still what a tap on them reaches.
  EXPECT_TRUE(TapReaches(done));
  EXPECT_TRUE(TapReaches(cancel));

  // The upload failed or finished: the pill goes, the buttons are back.
  flags.uploading = false;
  flags.locked = false;
  DataModelHost::Instance().Dirty("avatar_crop", "crop_uploading");
  DataModelHost::Instance().Dirty("avatar_crop", "crop_locked");
  context->Update();
  EXPECT_FALSE(pill_shown());
  for (ui::Element* e : {cancel, done}) {
    EXPECT_FALSE(e->IsClassSet("avatar-crop-btn--disabled"));
    EXPECT_FALSE(e->HasAttribute("disabled"));
  }
  document->Close();
  context->Update();
}

TEST_F(AvatarCropLayoutTest, OnTheDesktopRotateStaysAndThePhoneCloseIsNotThere) {
  ui::ElementDocument* document = Mount("avatar_crop.rml", /*compact=*/false);
  ASSERT_NE(document, nullptr);
  ui::Element* rotate = First(document, "avatar-crop-btn--rotate");
  ui::Element* close = First(document, "avatar-crop-close");
  ASSERT_NE(rotate, nullptr);
  ASSERT_NE(close, nullptr);
  EXPECT_TRUE(rotate->IsVisible(true));
  EXPECT_FALSE(close->IsVisible(true));
  document->Close();
  context->Update();
}

// The viewer has the same black layer behind it: the close button is below the top inset and the picture, centred in
// the space under it, is what a tap reaches (not the black).
TEST_F(AvatarCropLayoutTest, TheViewersCloseButtonAndPictureAreOnScreenAndReceiveTaps) {
  ui::ElementDocument* document = Mount("avatar_view.rml", /*compact=*/true);
  ASSERT_NE(document, nullptr);
  ui::Element* close = First(document, "avatar-view-close");
  ui::Element* image = First(document, "avatar-view-image");
  ui::Element* stage = First(document, "avatar-view-stage");
  ASSERT_NE(close, nullptr);
  ASSERT_NE(image, nullptr);
  ASSERT_NE(stage, nullptr);
  EXPECT_GE(Top(close), kTop);
  EXPECT_GE(H(close), 44.f);
  EXPECT_TRUE(TapReaches(close));
  EXPECT_FLOAT_EQ(W(image), 390.f);
  EXPECT_FLOAT_EQ(H(image), 390.f);
  EXPECT_GE(Top(image), Bottom(close) - 0.5f);
  EXPECT_LE(Bottom(image), 844.f - kBottom);
  EXPECT_NEAR(Top(image) + H(image) / 2, Top(stage) + H(stage) / 2, 0.5f) << "centred in the space under the button";
  EXPECT_TRUE(TapReaches(image));
  document->Close();
  context->Update();
}

} // namespace
} // namespace pbr
