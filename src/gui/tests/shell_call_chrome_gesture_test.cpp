// #94 (Kenneth 2026-11-06, iPhone): on the group video call page, swiping through the members shrank the whole call
// to its corner chip far too easily. On a phone the call page takes only one gesture, a pull down that shrinks it
// (ShellHost maps on_expand to minimize there). A real context with the gesture attached, the page's shape as in
// ShellCallChromeRml / components.rcss: a drag over the members must never shrink the call, a tap still reaches them.
#include "gui/shell/ShellCallChromeGesture.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/EventListener.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

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

/** Counts clicks that reach a member's tile (in the app: call_spotlight, which spotlights them). */
class ClickCounter : public ui::EventListener {
public:
  void ProcessEvent(ui::Event&) override { ++clicks; }
  int clicks = 0;
};

// 400×800: the group's name (0-100), the members' area (100-560: a 2×2 grid, 10px gaps), the strip (580-700: three
// small tiles, 10px gaps), the buttons (700-800).
constexpr const char* kPageRml = R"(<rml><head><style>
body { display: block; width: 400px; height: 800px; margin: 0; }
div { display: block; }
#shell-call-chrome-root { position: absolute; top: 0; left: 0; width: 400px; height: 800px; }
.call-phone-head { position: absolute; top: 0; left: 0; width: 400px; height: 100px; }
.call-phone-group-body { position: absolute; top: 100px; left: 0; width: 400px; height: 460px; }
.call-phone-grid { position: absolute; top: 0; left: 0; right: 0; bottom: 0; overflow-y: auto; }
.call-phone-tile { position: absolute; width: 195px; height: 225px; }
.call-phone-strip { position: absolute; top: 580px; left: 0; width: 400px; height: 120px; overflow: hidden; }
.call-phone-tile--strip { width: 120px; height: 120px; top: 0; }
.call-phone-controls { position: absolute; top: 700px; left: 0; width: 400px; height: 100px; }
</style></head><body>
<div id="shell-call-chrome-root"><div id="call-phone-page" class="call-phone call-phone--group">
  <div class="call-phone-head"></div>
  <div class="call-phone-group-body"><div class="call-phone-grid">
    <div id="tile" class="call-phone-tile" style="top: 0; left: 0;"></div>
    <div class="call-phone-tile" style="top: 0; left: 205px;"></div>
    <div class="call-phone-tile" style="top: 235px; left: 0;"></div>
    <div class="call-phone-tile" style="top: 235px; left: 205px;"></div>
  </div></div>
  <div class="call-phone-strip">
    <div id="strip-tile" class="call-phone-tile call-phone-tile--strip" style="left: 0;"></div>
    <div class="call-phone-tile call-phone-tile--strip" style="left: 130px;"></div>
    <div class="call-phone-tile call-phone-tile--strip" style="left: 260px;"></div>
  </div>
  <div class="call-phone-controls"></div>
</div></div></body></rml>)";

class ShellCallChromeGestureTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("call-chrome-gesture-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(kPageRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    tile_ = document_->GetElementById("tile");
    strip_tile_ = document_->GetElementById("strip-tile");
    ASSERT_NE(tile_, nullptr);
    ASSERT_NE(strip_tile_, nullptr);
    tile_->AddEventListener(ui::EventId::Click, &tile_clicks_);
    strip_tile_->AddEventListener(ui::EventId::Click, &strip_clicks_);
    // As ShellHost attaches it on a phone: the full page in Immersive, pull down = shrink.
    ShellCallChromeGesture::Callbacks callbacks;
    callbacks.on_expand = [this]() { ++shrinks_; };
    gesture_.Attach(document_->GetElementById("shell-call-chrome-root"), context_, CallChromeMode::Immersive,
                    std::move(callbacks), &axis_lock_);
  }

  void TearDown() override {
    gesture_.Detach();
    if (tile_) {
      tile_->RemoveEventListener(ui::EventId::Click, &tile_clicks_);
    }
    if (strip_tile_) {
      strip_tile_->RemoveEventListener(ui::EventId::Click, &strip_clicks_);
    }
    if (context_) {
      ui::RemoveContext("call-chrome-gesture-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void Drag(const int from_x, const int from_y, const int to_x, const int to_y) {
    context_->ProcessMouseMove(from_x, from_y, 0);
    context_->ProcessMouseButtonDown(0, 0);
    const int steps = 10;
    for (int i = 1; i <= steps; ++i) {
      context_->ProcessMouseMove(from_x + (to_x - from_x) * i / steps, from_y + (to_y - from_y) * i / steps, 0);
      context_->Update();
    }
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  void Tap(const int x, const int y) {
    context_->ProcessMouseMove(x, y, 0);
    context_->ProcessMouseButtonDown(0, 0);
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Element* tile_ = nullptr;
  ui::Element* strip_tile_ = nullptr;
  ClickCounter tile_clicks_;
  ClickCounter strip_clicks_;
  ShellGestureAxisLock axis_lock_;
  ShellCallChromeGesture gesture_;
  int shrinks_ = 0;
};

TEST_F(ShellCallChromeGestureTest, PullingDownOutsideTheMembersStillShrinksTheCall) {
  Drag(200, 40, 200, 240);
  EXPECT_EQ(shrinks_, 1);
}

TEST_F(ShellCallChromeGestureTest, NoDragOverTheMembersShrinksTheCall) {
  const struct {
    const char* where;
    int x, y, to_x, to_y;
  } drags[] = {
      {"down over a grid tile", 100, 150, 100, 450},
      {"down over the gap between tiles", 200, 200, 200, 500},
      {"sideways, drifting down, over a grid tile", 150, 150, 20, 300},
      {"down over a strip tile", 60, 600, 60, 790},
      {"sideways, drifting down, over the strip", 330, 600, 30, 700},
      {"down from the gap in the strip", 125, 600, 125, 790},
  };
  for (const auto& d : drags) {
    Drag(d.x, d.y, d.to_x, d.to_y);
    EXPECT_EQ(shrinks_, 0) << d.where;
    EXPECT_EQ(axis_lock_.Get(), ShellGestureAxis::None) << d.where;
  }
}

TEST_F(ShellCallChromeGestureTest, ATapOnAMemberReachesItAndADragDoesNot) {
  Tap(100, 150);
  EXPECT_EQ(tile_clicks_.clicks, 1) << "a tap spotlights the member";
  Tap(60, 640);
  EXPECT_EQ(strip_clicks_.clicks, 1) << "on the strip too";

  Drag(100, 150, 140, 300);
  EXPECT_EQ(tile_clicks_.clicks, 1) << "a swipe that starts and ends on a tile is not a tap on it";
  Tap(100, 150);
  EXPECT_EQ(tile_clicks_.clicks, 2) << "the next tap is a tap again";
  EXPECT_EQ(shrinks_, 0);
}

} // namespace
} // namespace pbr
