// DEV/PLAN-call-frame-cost.md step 4, through the real ShellHost: the layers under the phone call page hide when the
// page is mounted full screen and show at once on every path that ends or shrinks it, with no settings switch.
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/IAssetLocator.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellBaseHide.h"
#include "gui/shell/ShellCallChromePorts.h"
#include "gui/shell/ShellHost.h"
#include "lib/ui/ShellLayout.h"
#include "lib/ui/ViewCatalog.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 0; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class AssetsLocator : public IAssetLocator {
public:
  std::string Resolve(const std::string& relative) const override {
    return (std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative).string();
  }
};

constexpr const char* kShellRml = R"(<rml><head><style>
body { display: block; width: 400px; height: 800px; margin: 0; }
div, p, button, span { display: block; }
#shell-root { position: absolute; top: 0; left: 0; width: 400px; height: 800px; }
.shell-layer { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }
</style></head><body><div id="shell-root"></div><div id="shell-statusbar-strip" class="shell-statusbar-strip"></div>
<div id="shell-home-indicator-strip" class="shell-home-indicator-strip"></div></body></rml>)";

CallChromeSnapshot InCall(const CallChromeMode mode) {
  CallChromeSnapshot snapshot;
  snapshot.in_progress.active = true;
  snapshot.in_progress.mode = mode;
  snapshot.in_progress.mode_str = mode == CallChromeMode::Minimized   ? "minimized"
                                  : mode == CallChromeMode::Immersive ? "immersive"
                                                                      : "expanded";
  return snapshot;
}

class ShellBaseHideHostTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("shell-base-hide-host-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    AppRuntime::ShutdownUI(); // a fresh UI queue: tasks other tests left behind hold objects that are gone
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(&locator_);
    ViewCatalog::ClearCache();
    host_.Initialize(context_);
    ASSERT_TRUE(host_.RegisterWindowModel(context_));
    document_ = context_->LoadDocumentFromMemory(kShellRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    host_.State().layout_mode = LayoutMode::Compact;
    ShellLayout::SyncLayoutModeString(host_.State());
    host_.SelectNavTab(NavTab::Home);
    host_.SyncLayout();
    Settle();
  }

  void TearDown() override {
    host_.ApplyCallChromeSnapshot(CallChromeSnapshot{}, CallChromeUpdate::Remount);
    Settle();
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(nullptr);
    ViewCatalog::ClearCache();
    DataModelHost::Instance().Clear();
    ui::RemoveContext("shell-base-hide-host-test");
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
  }

  /** The next UI turns (the deferred remounts), then a layout pass. */
  void Settle() {
    for (int i = 0; i < 3; ++i) {
      AppRuntime::RunUITasks();
      context_->Update();
    }
  }

  bool Shown(const char* id) {
    ui::Element* element = document_->GetElementById(id);
    return element && element->IsVisible(true);
  }
  bool BaseShown() { return Shown("shell-base-layer"); }
  bool CallPageMounted() { return document_->GetElementById("call-phone-page") != nullptr; }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  AssetsLocator locator_;
  ShellHost host_;
  ui::ElementDocument* document_ = nullptr;
};

TEST_F(ShellBaseHideHostTest, AFullScreenCallPageHidesWhatItCoversOnceItIsMounted) {
  ASSERT_TRUE(BaseShown());
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Expanded), CallChromeUpdate::Remount);
  EXPECT_TRUE(BaseShown()) << "not before the call page is there";
  Settle();
  ASSERT_TRUE(CallPageMounted());
  EXPECT_TRUE(Shown("shell-call-in-progress-mount"));
  EXPECT_FALSE(BaseShown());
  EXPECT_FALSE(Shown("shell-ai-drawer-host"));
  EXPECT_TRUE(ShellBaseHide::Hidden());
}

TEST_F(ShellBaseHideHostTest, TheCallEndingShowsThePagesAtOnce) {
  // The presenter ending the call, the peer hanging up: either way the next snapshot has no active call.
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Expanded), CallChromeUpdate::Remount);
  Settle();
  ASSERT_FALSE(BaseShown());
  host_.ApplyCallChromeSnapshot(CallChromeSnapshot{}, CallChromeUpdate::Remount);
  EXPECT_TRUE(BaseShown()) << "before the remount runs";
  Settle();
  EXPECT_TRUE(BaseShown());
  EXPECT_FALSE(CallPageMounted());
  EXPECT_FALSE(ShellBaseHide::Hidden());
}

TEST_F(ShellBaseHideHostTest, ACallStateClearedWithoutARemountStillShowsThePages) {
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Immersive), CallChromeUpdate::Remount);
  Settle();
  ASSERT_FALSE(BaseShown());
  // Not the update kind a layer change gets: the pages must not wait for a remount that is not coming.
  host_.ApplyCallChromeSnapshot(CallChromeSnapshot{}, CallChromeUpdate::DirtyOnly);
  EXPECT_TRUE(BaseShown());
  Settle();
  EXPECT_TRUE(BaseShown());
}

TEST_F(ShellBaseHideHostTest, ShrinkingToTheChipShowsThePagesAndOpeningAgainHidesThem) {
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Expanded), CallChromeUpdate::Remount);
  Settle();
  ASSERT_FALSE(BaseShown());
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Minimized), CallChromeUpdate::Remount);
  EXPECT_TRUE(BaseShown());
  Settle();
  EXPECT_TRUE(BaseShown());
  EXPECT_TRUE(Shown("shell-call-in-progress-mount")) << "the chip";
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Expanded), CallChromeUpdate::Remount);
  Settle();
  EXPECT_FALSE(BaseShown());
}

TEST_F(ShellBaseHideHostTest, TheRingAndThePinPageOverTheCallStayVisible) {
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Expanded), CallChromeUpdate::Remount);
  Settle();
  CallChromeSnapshot second_call = InCall(CallChromeMode::Expanded);
  second_call.ring.active = true;
  host_.ApplyCallChromeSnapshot(second_call, CallChromeUpdate::Remount);
  PinGateState pin;
  pin.active = true;
  pin.unlock_mode = true;
  host_.ApplyPinGateState(pin);
  Settle();
  EXPECT_FALSE(BaseShown());
  EXPECT_TRUE(Shown("shell-call-ring-mount"));
  EXPECT_TRUE(Shown("shell-pin-gate-mount"));
  EXPECT_TRUE(Shown("shell-dialog-mount"));
  EXPECT_TRUE(Shown("shell-call-in-progress-mount"));
}

TEST_F(ShellBaseHideHostTest, TheDesktopLayoutNeverHides) {
  host_.State().layout_mode = LayoutMode::Expanded;
  ShellLayout::SyncLayoutModeString(host_.State());
  host_.SyncLayout();
  Settle();
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Expanded), CallChromeUpdate::Remount);
  Settle();
  EXPECT_FALSE(ShellBaseHide::Hidden());
  ui::ElementList bases;
  document_->GetElementsByClassName(bases, "shell-layer-base");
  ASSERT_FALSE(bases.empty());
  for (ui::Element* base : bases) {
    EXPECT_TRUE(base->IsVisible(true));
  }
}

TEST_F(ShellBaseHideHostTest, AShellRebuildDuringTheCallKeepsThePagesHidden) {
  host_.ApplyCallChromeSnapshot(InCall(CallChromeMode::Expanded), CallChromeUpdate::Remount);
  Settle();
  ASSERT_FALSE(BaseShown());
  host_.SyncLayout(); // a new message, a language change: the base layer is built again
  EXPECT_FALSE(BaseShown());
  Settle();
  EXPECT_FALSE(BaseShown());
  EXPECT_TRUE(CallPageMounted());
}

} // namespace
} // namespace pbr
