// #94 (Kenneth 2026-11-06, iPhone): on the group video call page, swiping through the members shrank the whole call
// to its corner chip far too easily. On a phone the call page takes only one gesture, a pull down that shrinks it
// (ShellHost maps on_expand to minimize there). A real context with the gesture attached, the page's shape as in
// ShellCallChromeRml / components.rcss: a drag over the members must never shrink the call, a tap still reaches them.
#include "gui/shell/ShellCallChromeGesture.h"
#include "gui/shell/ShellCallChromeRml.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/EventListener.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

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

// #166 (Kenneth 2026-11-08): the minimised chip is let go anywhere and rests on the nearer side at that height, sliding
// there from where the finger left it (CallChipDock). A real context with the chip as ShellCallChromeRml mounts it, the
// default corner as in components.rcss, and a clock the test moves (the slide's animation runs on it).
class FakeClock : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return now; }
  double now = 100.0;
};

// 400×800 layer; the chip 180×56 at the default top-right corner: left 208, top 12. The band for its top: [12, 672].
constexpr const char* kChipRml = R"(<rml><head><style>
body { display: block; width: 400px; height: 800px; margin: 0; }
div { display: block; }
#layer { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }
.shell-call-minimized-chip { position: absolute; width: 180px; height: 56px; }
.shell-call-minimized-chip--tr { top: 12dp; right: 12dp; }
</style></head><body>
<div id="layer"><div id="shell-call-minimized-chip" class="shell-call-minimized-chip shell-call-minimized-chip--tr">
<div id="chip-title"></div></div></div>
</body></rml>)";

class ShellCallChipDragTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    ui::SetSystemInterface(&clock_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("call-chip-drag-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(kChipRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    chip_ = document_->GetElementById("shell-call-minimized-chip");
    ASSERT_NE(chip_, nullptr);
    call_.active = true;
    call_.mode = CallChromeMode::Minimized;
    ShellCallChromeGesture::Callbacks callbacks;
    callbacks.on_chip_place = [this](int side, int y_permille) { placed_ = CallChipPlace{side, y_permille}; };
    callbacks.on_chip_settled = [this]() { ++settled_; };
    callbacks.on_restore = [this]() { ++restores_; };
    gesture_.Attach(chip_, context_, CallChromeMode::Minimized, std::move(callbacks));
  }

  void TearDown() override {
    gesture_.Detach();
    if (context_) {
      ui::RemoveContext("call-chip-drag-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
  }

  /** Press at (x, y), move to (to_x, to_y) a frame at a time; let go unless `hold`. */
  void Drag(const int x, const int y, const int to_x, const int to_y, const bool hold = false) {
    context_->ProcessMouseMove(x, y, 0);
    context_->ProcessMouseButtonDown(0, 0);
    const int steps = 10;
    for (int i = 1; i <= steps; ++i) {
      context_->ProcessMouseMove(x + (to_x - x) * i / steps, y + (to_y - y) * i / steps, 0);
      context_->Update();
    }
    if (!hold) {
      context_->ProcessMouseButtonUp(0, 0);
    }
  }

  /** `seconds` of frames (an animation advances at most 0.1s a frame, so a long gap is cut into 50ms frames). */
  void Frame(const double seconds) {
    double left = seconds;
    do {
      const double step = left > 0.05 ? 0.05 : left;
      clock_.now += step;
      left -= step;
      context_->Update();
    } while (left > 1e-9);
  }

  ui::Vector2f At() { return chip_->GetAbsoluteOffset(ui::BoxArea::Border); }

  std::string Local(const char* property) {
    const ui::Property* value = chip_->GetLocalProperty(property);
    return value ? value->ToString() : std::string();
  }

  NullRender renderer_;
  FakeClock clock_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Element* chip_ = nullptr;
  CallInProgressState call_;
  ShellCallChromeGesture gesture_;
  std::optional<CallChipPlace> placed_;
  int settled_ = 0;
  int restores_ = 0;
};

TEST_F(ShellCallChipDragTest, LetGoItStaysWhereItShowsThenSlidesToTheNearerSide) {
  // Grabbed at (300, 40), let go 50 right and 360 down: it shows at (258, 372), its centre in the right half.
  Drag(300, 40, 350, 400);
  // At once (no frame yet): pinned by left / top at the drop point, the translate gone, the corner class gone.
  EXPECT_EQ(Local("left"), "258dp");
  EXPECT_EQ(Local("top"), "372dp");
  EXPECT_EQ(Local("right"), "auto");
  EXPECT_EQ(Local("bottom"), "auto");
  EXPECT_EQ(Local("transform"), "translate(0dp, 0dp)");
  EXPECT_FALSE(chip_->IsClassSet("shell-call-minimized-chip--tr"));
  ASSERT_TRUE(placed_.has_value());
  EXPECT_EQ(placed_->side, CallChipDock::kSideRight);
  EXPECT_EQ(placed_->y_permille, 545); // (372 - 12) / (800 - 72 - 56 - 12)
  EXPECT_TRUE(gesture_.IsSettling());

  // The first frame lays it out where it showed under the finger: no jump.
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 258.f);
  EXPECT_FLOAT_EQ(At().y, 372.f);

  // Mid-slide: on its way to the right edge (left 400 - 12 - 180 = 208), the height kept.
  Frame(0.1);
  EXPECT_LT(At().x, 258.f);
  EXPECT_GT(At().x, 208.f);
  EXPECT_EQ(settled_, 0);

  // There: anchored by `right` (a chip that grows grows away from the edge), at the same place.
  const float top = CallChipDock::Place(*placed_, {400.f, 800.f}, 180.f, 56.f).top_dp;
  Frame(0.2);
  EXPECT_FALSE(gesture_.IsSettling());
  EXPECT_EQ(settled_, 1);
  EXPECT_EQ(Local("right"), "12dp");
  EXPECT_EQ(Local("left"), "auto");
  EXPECT_EQ(Local("bottom"), "auto");
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 208.f);
  EXPECT_NEAR(At().y, top, 0.1f);
  EXPECT_NEAR(At().y, 372.f, (672.f - 12.f) / 1000.f);
  EXPECT_EQ(restores_, 0);
}

TEST_F(ShellCallChipDragTest, ToTheLeftSideItIsAnchoredByLeft) {
  Drag(300, 40, 60, 300); // shows at (-32, 272): the left half
  ASSERT_TRUE(placed_.has_value());
  EXPECT_EQ(placed_->side, CallChipDock::kSideLeft);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, -32.f);
  Frame(0.3);
  EXPECT_EQ(Local("left"), "12dp");
  EXPECT_EQ(Local("right"), "auto");
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 12.f);
  EXPECT_NEAR(At().y, 272.f, 1.f);

  // And back to the right: no stretch (left and right are never both set), no jump at the drop.
  Drag(60, 290, 330, 290); // shows at (282, 272)
  EXPECT_EQ(placed_->side, CallChipDock::kSideRight);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 282.f);
  EXPECT_FLOAT_EQ(chip_->GetBox().GetSize(ui::BoxArea::Border).x, 180.f);
  Frame(0.3);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 208.f);
  EXPECT_FLOAT_EQ(chip_->GetBox().GetSize(ui::BoxArea::Border).x, 180.f);
}

TEST_F(ShellCallChipDragTest, APressMidSlideStopsItWhereItShows) {
  Drag(300, 40, 350, 400);
  Frame(0.0);
  Frame(0.1);
  const ui::Vector2f caught = At();
  ASSERT_LT(caught.x, 258.f);
  ASSERT_GT(caught.x, 208.f);
  // The finger comes down on it mid-slide: the slide stops there, now and later.
  context_->ProcessMouseMove(static_cast<int>(caught.x) + 20, static_cast<int>(caught.y) + 20, 0);
  context_->ProcessMouseButtonDown(0, 0);
  EXPECT_FALSE(gesture_.IsSettling());
  Frame(0.05);
  EXPECT_NEAR(At().x, caught.x, 0.1f);
  EXPECT_NEAR(At().y, caught.y, 0.1f);
  Frame(0.5);
  EXPECT_NEAR(At().x, caught.x, 0.1f) << "the old slide moves it no more";
  EXPECT_EQ(settled_, 0);
  // It follows the finger from there (the translate on top of where it stopped) and is let go again.
  for (int i = 1; i <= 5; ++i) {
    context_->ProcessMouseMove(static_cast<int>(caught.x) + 20 - 30 * i, static_cast<int>(caught.y) + 20, 0);
    context_->Update();
  }
  context_->ProcessMouseButtonUp(0, 0);
  EXPECT_NEAR(std::stof(Local("left")), caught.x - 150.f, 0.1f) << "dropped where the finger left it";
  Frame(0.0);
  EXPECT_NEAR(At().x, caught.x - 150.f, 0.1f);
  Frame(0.3);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 12.f);
  EXPECT_EQ(settled_, 1);
}

TEST_F(ShellCallChipDragTest, ATapStillRestoresTheCall) {
  context_->ProcessMouseMove(300, 40, 0);
  context_->ProcessMouseButtonDown(0, 0);
  context_->ProcessMouseButtonUp(0, 0);
  EXPECT_EQ(restores_, 1);
  EXPECT_FALSE(placed_.has_value());
  EXPECT_TRUE(chip_->IsClassSet("shell-call-minimized-chip--tr"));
}

// A remount (ShellHost::RemountCallChromeNow, as the clock's status or the dropped place come back) rebuilds the chip
// from the call state: never while a drag owns it, nor while the state does not have the dropped place yet.
TEST_F(ShellCallChipDragTest, NoRebuildWhileItSlidesOrBeforeTheStateHasThePlace) {
  ui::Element* layer = document_->GetElementById("layer");
  const auto remount = [&]() {
    if (KeepMinimizedChipOnRemount(call_, document_->GetElementById("shell-call-minimized-chip"), gesture_)) {
      return;
    }
    layer->SetInnerRML(
        "<div id=\"shell-call-minimized-chip\" class=\"shell-call-minimized-chip "
        "shell-call-minimized-chip--tr\"></div>");
  };

  EXPECT_FALSE(KeepMinimizedChipOnRemount(call_, chip_, gesture_)) << "never dragged: rebuilt as ever";

  Drag(300, 40, 350, 400, /*hold=*/true);
  EXPECT_TRUE(KeepMinimizedChipOnRemount(call_, chip_, gesture_)) << "under the finger";
  context_->ProcessMouseButtonUp(0, 0);
  ASSERT_TRUE(placed_.has_value());
  Frame(0.0);
  Frame(0.1);
  const ui::Vector2f sliding = At();

  // The slide runs and the state still has the old place (side 0): the chip stays the same element, where it is.
  remount();
  EXPECT_EQ(document_->GetElementById("shell-call-minimized-chip"), chip_);
  EXPECT_FLOAT_EQ(At().x, sliding.x);

  // The place came back mid-slide: still held until the slide ends.
  call_.minimized_side = placed_->side;
  call_.minimized_y_permille = placed_->y_permille;
  EXPECT_TRUE(KeepMinimizedChipOnRemount(call_, chip_, gesture_));

  // Slid, but the state lags (the old place): held.
  call_.minimized_side = 0;
  call_.minimized_y_permille = 0;
  Frame(0.3);
  ASSERT_FALSE(gesture_.IsSettling());
  remount();
  EXPECT_EQ(document_->GetElementById("shell-call-minimized-chip"), chip_);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 208.f);

  // A restore is a new mode: never held.
  call_.mode = CallChromeMode::Expanded;
  EXPECT_FALSE(KeepMinimizedChipOnRemount(call_, chip_, gesture_));
  call_.mode = CallChromeMode::Minimized;

  // The state has it: the rebuild runs (its markup then carries the same place, ShellCallChromeRml).
  call_.minimized_side = placed_->side;
  call_.minimized_y_permille = placed_->y_permille;
  EXPECT_FALSE(KeepMinimizedChipOnRemount(call_, chip_, gesture_));
  gesture_.Detach(); // as RemountCallChromeNow does before it mounts
  remount();
  EXPECT_NE(document_->GetElementById("shell-call-minimized-chip"), nullptr);
}

TEST_F(ShellCallChipDragTest, ReflowKeepsADroppedChipInItsBandAsItGrows) {
  Drag(300, 40, 350, 760); // let go low: the bottom of the band
  ASSERT_TRUE(placed_.has_value());
  EXPECT_EQ(placed_->y_permille, 1000);
  Frame(0.0);
  Frame(0.3);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().y, 800.f - 72.f - 56.f);
  call_.minimized_side = placed_->side;
  call_.minimized_y_permille = placed_->y_permille;
  // The chip grows (the quality words): the next frame keeps its bottom 72dp above the layer's.
  chip_->SetProperty("height", "80px");
  Frame(0.0);
  gesture_.Reflow(call_);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().y, 800.f - 72.f - 80.f);
  EXPECT_EQ(Local("right"), "12dp");

  // It reads the chip's box again only every 250ms (not each frame): a change shows within that.
  chip_->SetProperty("height", "100px");
  Frame(0.0);
  gesture_.Reflow(call_);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().y, 800.f - 72.f - 80.f) << "within 250ms of the last look";
  Frame(0.3);
  gesture_.Reflow(call_);
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().y, 800.f - 72.f - 100.f);
}

// A slide whose end never comes (no Animationend: the animation did not run, or the chip was hidden) is ended by the
// frame's Reflow 0.4s after the drop: anchored on its side, and its rebuild no longer held.
TEST_F(ShellCallChipDragTest, ASlideWhoseEndNeverComesIsEndedByReflow) {
  Drag(300, 40, 350, 400);
  ASSERT_TRUE(placed_.has_value());
  ASSERT_TRUE(gesture_.IsSettling());
  call_.minimized_side = placed_->side;
  call_.minimized_y_permille = placed_->y_permille;
  // No context update from here: the animation never advances, so its Animationend never comes.
  clock_.now += 0.2;
  gesture_.Reflow(call_);
  EXPECT_TRUE(gesture_.IsSettling()) << "not before its deadline";
  EXPECT_TRUE(KeepMinimizedChipOnRemount(call_, chip_, gesture_));
  clock_.now += 0.25;
  gesture_.Reflow(call_);
  EXPECT_FALSE(gesture_.IsSettling());
  EXPECT_EQ(settled_, 1);
  EXPECT_EQ(Local("right"), "12dp");
  EXPECT_EQ(Local("left"), "auto");
  EXPECT_FALSE(KeepMinimizedChipOnRemount(call_, chip_, gesture_));
}

// The chip as ShellCallChromeRml mounts it (inside its full-screen layer, #shell-call-chrome-root), the gesture attached
// the way ShellHost does (CallChromeGestureElement): the drop, the slide, the hold and the rendered place are all the
// chip's; the layer is never moved.
constexpr const char* kMountHead = R"(<rml><head><style>
body { display: block; width: 400px; height: 800px; margin: 0; }
div, p { display: block; }
#shell-call-in-progress-mount { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }
.shell-layer-call-minimized { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }
.shell-call-minimized-chip { position: absolute; width: 180px; height: 56px; overflow: hidden; }
.shell-call-minimized-chip--tr { top: 12dp; right: 12dp; }
</style></head><body><div id="shell-call-in-progress-mount">)";

class ShellCallChipMountedTest : public ShellCallChipDragTest {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    ui::SetSystemInterface(&clock_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("call-chip-drag-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    state_.call_in_progress.active = true;
    state_.call_in_progress.mode = CallChromeMode::Minimized;
    call_ = state_.call_in_progress;
    document_ =
        context_->LoadDocumentFromMemory(std::string(kMountHead) + CallInProgressRml(state_) + "</div></body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    layer_ = document_->GetElementById("shell-call-chrome-root");
    chip_ = document_->GetElementById("shell-call-minimized-chip");
    ASSERT_NE(layer_, nullptr);
    ASSERT_NE(chip_, nullptr);
    ASSERT_EQ(chip_->GetParentNode(), layer_);
  }

  void Attach() {
    ShellCallChromeGesture::Callbacks callbacks;
    callbacks.on_chip_place = [this](int side, int y_permille) { placed_ = CallChipPlace{side, y_permille}; };
    callbacks.on_chip_settled = [this]() { ++settled_; };
    callbacks.on_restore = [this]() { ++restores_; };
    gesture_.Attach(CallChromeGestureElement(document_, CallChromeMode::Minimized), context_, CallChromeMode::Minimized,
                    std::move(callbacks));
  }

  ShellState state_;
  ui::Element* layer_ = nullptr;
};

TEST_F(ShellCallChipMountedTest, TheGestureMovesTheChipNotItsLayer) {
  EXPECT_EQ(CallChromeGestureElement(document_, CallChromeMode::Minimized), chip_);
  EXPECT_EQ(CallChromeGestureElement(document_, CallChromeMode::Expanded), layer_);
  Attach();
  EXPECT_TRUE(gesture_.AttachedTo(chip_));
  EXPECT_FALSE(gesture_.AttachedTo(layer_));
  EXPECT_FLOAT_EQ(At().x, 208.f);
  EXPECT_FLOAT_EQ(At().y, 12.f);

  Drag(300, 40, 350, 400);
  ASSERT_TRUE(placed_.has_value());
  EXPECT_EQ(placed_->side, CallChipDock::kSideRight);
  EXPECT_EQ(placed_->y_permille, 545) << "measured on the 180x56 chip in the 400x800 layer";
  EXPECT_EQ(Local("left"), "258dp");
  EXPECT_EQ(Local("width"), "180dp");
  EXPECT_FALSE(chip_->IsClassSet("shell-call-minimized-chip--tr"));
  EXPECT_TRUE(layer_->GetLocalStyleProperties().empty()) << "no inline left / top / width / transform on the layer";

  // The hold, as ShellHost asks it (the chip looked up by id), while it slides and the state lags.
  EXPECT_TRUE(KeepMinimizedChipOnRemount(call_, document_->GetElementById("shell-call-minimized-chip"), gesture_));
  Frame(0.0);
  EXPECT_FLOAT_EQ(At().x, 258.f);
  Frame(0.3);
  EXPECT_EQ(settled_, 1) << "the chip's own Animationend";
  EXPECT_EQ(Local("right"), "12dp");
  Frame(0.0);
  EXPECT_TRUE(layer_->GetLocalStyleProperties().empty());
  EXPECT_FLOAT_EQ(layer_->GetAbsoluteOffset(ui::BoxArea::Border).x, 0.f);
  EXPECT_FLOAT_EQ(layer_->GetBox().GetSize(ui::BoxArea::Border).x, 400.f);
  EXPECT_FLOAT_EQ(At().x, 208.f);
  const float docked_top = At().y;

  // The place comes back and the chip is rebuilt from it (RemountCallChromeNow: measure, detach, mount): the rendered
  // inline place is on the chip, where it already was; the layer has none.
  state_.call_in_progress.minimized_side = placed_->side;
  state_.call_in_progress.minimized_y_permille = placed_->y_permille;
  call_ = state_.call_in_progress;
  EXPECT_FALSE(KeepMinimizedChipOnRemount(call_, chip_, gesture_));
  RememberCallChipLayout(gesture_, state_.call_chip_layout);
  gesture_.Detach();
  document_->GetElementById("shell-call-in-progress-mount")->SetInnerRML(CallInProgressRml(state_));
  context_->Update();
  layer_ = document_->GetElementById("shell-call-chrome-root");
  chip_ = document_->GetElementById("shell-call-minimized-chip");
  ASSERT_NE(chip_, nullptr);
  EXPECT_EQ(Local("right"), "12dp");
  EXPECT_EQ(Local("left"), "auto");
  EXPECT_EQ(Local("bottom"), "auto");
  EXPECT_TRUE(layer_->GetLocalStyleProperties().empty()) << "the rendered place is the chip's, not the layer's";
  EXPECT_FLOAT_EQ(At().x, 208.f);
  EXPECT_NEAR(At().y, docked_top, 0.1f);
}

// #166 (iPhone, 2026-11-08): dragged down and let go, the chip showed at the top of its side and then moved down to the
// drop. As on the device: the chip is rebuilt from the state while the ShellState has no heights measured (0) — mid
// slide (a full layout rebuild is never held) and after it. It must come back at the drop's target, never at 12dp.
TEST_F(ShellCallChipMountedTest, ARebuildAfterTheDropLandsOnItsTargetNotTheTopCorner) {
  Attach();
  Drag(300, 40, 350, 400); // down the right side
  ASSERT_TRUE(placed_.has_value());
  const float target = CallChipDock::Place(*placed_, {400.f, 800.f}, 180.f, 56.f).top_dp;
  Frame(0.0);
  Frame(0.1);
  state_.call_in_progress.minimized_side = placed_->side;
  state_.call_in_progress.minimized_y_permille = placed_->y_permille;
  ASSERT_EQ(state_.call_chip_layout.area_height_dp, 0.f);
  ASSERT_EQ(state_.call_chip_layout.chip_height_dp, 0.f);

  // RemountCallChromeNow: remember the layout (ShellHost::MeasureCallChip), detach, mount from the state.
  RememberCallChipLayout(gesture_, state_.call_chip_layout);
  const std::string rml = CallInProgressRml(state_);
  char expected[32];
  std::snprintf(expected, sizeof(expected), "top:%.1fdp;", target);
  EXPECT_NE(rml.find(expected), std::string::npos) << rml;
  EXPECT_EQ(rml.find("top:12.0dp"), std::string::npos);
  gesture_.Detach();
  document_->GetElementById("shell-call-in-progress-mount")->SetInnerRML(rml);
  context_->Update();
  chip_ = document_->GetElementById("shell-call-minimized-chip");
  ASSERT_NE(chip_, nullptr);
  // Its first frame: already at the target, on its side.
  EXPECT_FLOAT_EQ(At().x, 208.f);
  EXPECT_NEAR(At().y, target, 0.1f);

  // Attached again, its first Reflow finds nothing to correct, and the next rebuild (from the state alone, the new
  // gesture has no layout of its own yet) lands there too.
  Attach();
  gesture_.Reflow(state_.call_in_progress);
  Frame(0.0);
  EXPECT_NEAR(At().y, target, 0.1f);
  gesture_.Detach();
  ShellState fresh = state_;
  RememberCallChipLayout(gesture_, fresh.call_chip_layout); // nothing: keeps what the state had
  EXPECT_NE(CallInProgressRml(fresh).find(expected), std::string::npos);
}

// Reflow's first correction after a rebuild (here: rendered with no height known, so with no top) puts the chip in
// place at once: no slide from where the rebuild left it.
TEST_F(ShellCallChipMountedTest, ReflowsFirstCorrectionAfterARebuildIsNotAnimated) {
  state_.call_in_progress.minimized_side = CallChipDock::kSideRight;
  state_.call_in_progress.minimized_y_permille = 545;
  call_ = state_.call_in_progress;
  document_->GetElementById("shell-call-in-progress-mount")->SetInnerRML(CallInProgressRml(state_));
  chip_ = document_->GetElementById("shell-call-minimized-chip");
  ASSERT_NE(chip_, nullptr);
  Attach();
  // Just mounted, not laid out: nothing to measure; the very next frame tries again (not 250ms later).
  gesture_.Reflow(call_);
  context_->Update();
  gesture_.Reflow(call_);
  const float target = CallChipDock::Place({CallChipDock::kSideRight, 545}, {400.f, 800.f}, 180.f, 56.f).top_dp;
  // Set, not slid: the value is there before any frame runs an animation (rendered with no top, a slide would leave
  // none until a frame), and the first frame shows it.
  ASSERT_FALSE(Local("top").empty());
  EXPECT_NEAR(std::stof(Local("top")), target, 0.1f);
  Frame(0.0);
  EXPECT_NEAR(At().y, target, 0.1f);
  EXPECT_FALSE(gesture_.IsSettling());
  EXPECT_EQ(settled_, 0);
}

} // namespace
} // namespace pbr
