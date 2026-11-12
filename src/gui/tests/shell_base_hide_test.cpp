// Measurement switch (DEV/PLAN-call-frame-cost.md step 4): the layers a full-screen phone call page covers are hidden
// only while the switch is on AND the call page is full-screen; the layers above the call page are never touched.
#include "gui/shell/ShellBaseHide.h"
#include "gui/shell/ShellLayoutRml.h"
#include "lib/ui/ShellTypes.h"

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

ShellState PhoneStateWithCall(const CallChromeMode mode, const bool active = true) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_in_progress.active = active;
  state.call_in_progress.mode = mode;
  PaneState page;
  page.spec.key = "contact_detail";
  page.id = 1;
  state.transient_stack.push_back(page); // a page pushed over the tab: the transient layer exists
  state.overlay_stack.push_back({1, OverlayKind::Generic, "dialog", "views/dialog.rml", {}});
  return state;
}

TEST(ShellBaseHideTest, HidesOnlyWithTheSwitchOnAndAFullScreenCallPage) {
  EXPECT_TRUE(ShellBaseHide::ShouldHide(true, PhoneStateWithCall(CallChromeMode::Expanded)));
  EXPECT_TRUE(ShellBaseHide::ShouldHide(true, PhoneStateWithCall(CallChromeMode::Immersive)));
  EXPECT_FALSE(ShellBaseHide::ShouldHide(false, PhoneStateWithCall(CallChromeMode::Expanded))) << "switch off";
  EXPECT_FALSE(ShellBaseHide::ShouldHide(true, PhoneStateWithCall(CallChromeMode::Minimized))) << "small window";
  EXPECT_FALSE(ShellBaseHide::ShouldHide(true, PhoneStateWithCall(CallChromeMode::Expanded, false))) << "call ended";
  ShellState desktop = PhoneStateWithCall(CallChromeMode::Expanded);
  desktop.layout_mode = LayoutMode::Expanded;
  EXPECT_FALSE(ShellBaseHide::ShouldHide(true, desktop)) << "desktop's call bar covers nothing";
}

class ShellBaseHideDomTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("shell-base-hide-test", {390, 844});
    ASSERT_NE(context, nullptr);
    ShellState state = PhoneStateWithCall(CallChromeMode::Expanded);
    std::string root = ShellLayoutRml(state, false, "").ShellRoot();
    root = std::regex_replace(root, std::regex("\\s+data-[a-z-]+=\"[^\"]*\""), ""); // no data models here
    const std::string rml = "<rml><head></head><body><div id=\"shell-root\">" + root + "</div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("shell-base-hide-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  bool Shown(const char* id) {
    ui::Element* element = document->GetElementById(id);
    EXPECT_NE(element, nullptr) << id;
    return element && element->IsVisible(true);
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
};

TEST_F(ShellBaseHideDomTest, EveryCoveredLayerHidesAndComesBack) {
  for (const char* id : ShellBaseHide::kLayerIds) {
    EXPECT_TRUE(Shown(id)) << id << " before";
  }
  ShellBaseHide::Apply(context, document, true);
  context->Update();
  EXPECT_TRUE(ShellBaseHide::Hidden());
  EXPECT_FALSE(Shown("shell-base-layer"));
  EXPECT_FALSE(Shown("shell-ai-drawer-host"));
  EXPECT_FALSE(Shown("shell-transient-layer"));
  ShellBaseHide::Apply(context, document, false);
  context->Update();
  EXPECT_FALSE(ShellBaseHide::Hidden());
  for (const char* id : ShellBaseHide::kLayerIds) {
    EXPECT_TRUE(Shown(id)) << id << " after";
  }
}

TEST_F(ShellBaseHideDomTest, TheLayersAboveTheCallPageAreNotTouched) {
  ShellBaseHide::Apply(context, document, true);
  context->Update();
  // The overlay layer (dialogs, pages and sheets over the call), the call mounts, the dialog / photo / PIN mounts.
  ui::ElementList overlays;
  document->GetElementsByClassName(overlays, "shell-layer-overlay");
  ASSERT_FALSE(overlays.empty());
  for (ui::Element* overlay : overlays) {
    EXPECT_TRUE(overlay->IsVisible(true)) << "overlay layer";
  }
  for (const char* id : {"shell-call-in-progress-mount", "shell-call-ring-mount", "shell-dialog-mount",
                         "shell-photo-mount", "shell-pin-gate-mount", "shell-onboarding-mount"}) {
    EXPECT_TRUE(Shown(id)) << id;
  }
  ShellBaseHide::Apply(context, document, false);
}

} // namespace
} // namespace pbr
