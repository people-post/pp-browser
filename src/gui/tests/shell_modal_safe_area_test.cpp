// A modal desktop panel (the forward-progress card, the news reader) dims the whole window: through the real
// ShellHost::ApplySafeAreaLayout its scrim and stage reach out of #shell-root over the title bar's strip and the
// status bar (wide.rcss promises both). The layout fixtures of the forward and news tests copy that step; this one
// runs the host's own.
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/IAssetLocator.h"
#include "gui/shell/DataModelHost.h"
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
body { display: block; width: 1200px; height: 800px; margin: 0; }
div { display: block; }
#shell-root { position: absolute; top: 0; left: 0; width: 1200px; height: 800px; }
</style></head><body><div id="shell-root"></div>
<div id="shell-modal-scrim-7"></div><div id="shell-modal-stage-7"></div></body></rml>)";

class ShellModalSafeAreaTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    ASSERT_TRUE(ui::Initialise());
    context_ = ui::CreateContext("shell-modal-safe-area-test", {1200, 800});
    ASSERT_NE(context_, nullptr);
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(&locator_);
    ViewCatalog::ClearCache();
    host_.Initialize(context_);
    ASSERT_TRUE(host_.RegisterWindowModel(context_));
    document_ = context_->LoadDocumentFromMemory(kShellRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
  }

  void TearDown() override {
    AppRuntime::ShutdownUI();
    AppRuntime::InitializeUI();
    IAssetLocator::SetInstance(nullptr);
    ViewCatalog::ClearCache();
    DataModelHost::Instance().Clear();
    ui::RemoveContext("shell-modal-safe-area-test");
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
  }

  std::string Property(const char* id, const char* name) {
    ui::Element* element = document_->GetElementById(id);
    const ui::Property* value = element ? element->GetProperty(name) : nullptr;
    return value ? value->ToString() : "(unset)";
  }

  NullRender renderer_;
  ui::Context* context_ = nullptr;
  AssetsLocator locator_;
  ShellHost host_;
  ui::ElementDocument* document_ = nullptr;
};

TEST_F(ShellModalSafeAreaTest, AModalLayerOnTheWideWindowReachesOverTheTitleBarAndTheStatusBar) {
  const ShellConfig config{};
  host_.State().layout_mode = LayoutMode::Expanded;
  ShellLayout::SyncLayoutModeString(host_.State());
  host_.State().titlebar_visible = true;
  host_.State().statusbar_visible = true;
  OverlayEntry layer;
  layer.id = 7;
  layer.key = "forward_progress";
  layer.modal = true;
  host_.State().overlay_stack.push_back(layer);

  host_.SetSafeAreaInsetsFromPrefs(0, 0);

  const std::string top = "-" + std::to_string(static_cast<int>(config.titlebar_height_dp)) + "dp";
  const std::string bottom = "-" + std::to_string(static_cast<int>(config.statusbar_height_dp)) + "dp";
  for (const char* id : {"shell-modal-scrim-7", "shell-modal-stage-7"}) {
    EXPECT_EQ(Property(id, "top"), top) << id << ": up over the title bar's strip";
    EXPECT_EQ(Property(id, "bottom"), bottom) << id << ": down over the status bar";
  }
}

TEST_F(ShellModalSafeAreaTest, ALayerThatIsNotModalLeavesTheScrimAlone) {
  host_.State().layout_mode = LayoutMode::Expanded;
  ShellLayout::SyncLayoutModeString(host_.State());
  host_.State().titlebar_visible = true;
  host_.State().statusbar_visible = true;
  OverlayEntry layer;
  layer.id = 7;
  layer.key = "forward_progress";
  host_.State().overlay_stack.push_back(layer);

  host_.SetSafeAreaInsetsFromPrefs(0, 0);

  EXPECT_EQ(Property("shell-modal-scrim-7", "top"), "auto");
}

} // namespace
} // namespace pbr
