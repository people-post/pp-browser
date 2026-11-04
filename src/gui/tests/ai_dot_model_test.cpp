// The phone AI dot (DESIGN-pp-ui §8.14, PLAN-ui-batch5-ask-ai 5c): drag anywhere, stick to the nearer side, tuck
// half away after 4.5 s, a short idle move every 5–10 s, and hidden wherever it would be in the way.
#include "gui/shell/AiDotModel.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace pbr {
namespace {

// An iPhone 16 Pro in dp: 402 × 874, 62 under the status bar, 34 over the home indicator; the medium dot.
constexpr AiDotArea kArea{402.f, 874.f, 62.f, 34.f, AiDotModel::kMediumDp};
constexpr float kSize = kArea.size_dp;
const float kGap = AiDotModel::EdgeGapDp(kSize);

AiDotScene Home() { return AiDotScene{}; }

TEST(AiDotModelTest, ShowsOnAPhonePageWhenSwitchedOn) {
  EXPECT_TRUE(AiDotModel::Shown(Home()));
  AiDotScene off = Home();
  off.enabled = false;
  EXPECT_FALSE(AiDotModel::Shown(off)) << "Me → AI assistant → Show AI dot is off";
  AiDotScene desktop = Home();
  desktop.phone = false;
  EXPECT_FALSE(AiDotModel::Shown(desktop)) << "no floating dot on the desktop layout";
}

TEST(AiDotModelTest, HidesDuringACallTheScannerThePinGateAndSecretPages) {
  const auto hidden_with = [](bool AiDotScene::*field) {
    AiDotScene scene = Home();
    scene.*field = true;
    return !AiDotModel::Shown(scene);
  };
  EXPECT_TRUE(hidden_with(&AiDotScene::call)) << "ring, call screen, call bar";
  EXPECT_TRUE(hidden_with(&AiDotScene::scanning));
  EXPECT_TRUE(hidden_with(&AiDotScene::pin_gate));
  EXPECT_TRUE(hidden_with(&AiDotScene::secret_page)) << "change PIN, recovery phrase";
  EXPECT_TRUE(hidden_with(&AiDotScene::cover)) << "startup cover, onboarding";
}

TEST(AiDotModelTest, HidesWhileTheKeyboardIsUpOrASheetIsOpen) {
  AiDotScene keyboard = Home();
  keyboard.keyboard_up = true;
  EXPECT_FALSE(AiDotModel::Shown(keyboard));
  AiDotScene sheet = Home();
  sheet.layer_open = true;
  EXPECT_FALSE(AiDotModel::Shown(sheet)) << "the ask-AI panel the dot opens is a sheet too";
}

TEST(AiDotModelTest, RestsAtTheRightEdgeInsideTheSafeArea) {
  const AiDotPoint at = AiDotModel::Rest(AiDotPlace{}, kArea, /*tucked=*/false);
  EXPECT_FLOAT_EQ(at.x_dp, 402.f - kSize - kGap);
  EXPECT_GE(at.y_dp, kArea.safe_top_dp + AiDotModel::kTopReserveDp);
  EXPECT_LE(at.y_dp + kSize, kArea.height_dp - kArea.safe_bottom_dp - AiDotModel::kBottomReserveDp);
}

TEST(AiDotModelTest, ThreeSizesMediumByDefault) {
  EXPECT_FLOAT_EQ(AiDotModel::SizeDp("s"), 52.f);
  EXPECT_FLOAT_EQ(AiDotModel::SizeDp("m"), 64.f);
  EXPECT_FLOAT_EQ(AiDotModel::SizeDp("l"), 76.f);
  EXPECT_FLOAT_EQ(AiDotModel::SizeDp(""), 64.f) << "an unknown size is the medium one";
  EXPECT_FLOAT_EQ(AiDotArea{}.size_dp, 64.f);
  EXPECT_FLOAT_EQ(AiDotModel::EdgeGapDp(56.f), 8.f) << "the first 56dp dot's gap";
}

TEST(AiDotModelTest, TheTuckTheEdgeGapAndTheClampsFollowTheSize) {
  for (const char* size : {"s", "m", "l"}) {
    AiDotArea area = kArea;
    area.size_dp = AiDotModel::SizeDp(size);
    const float side = area.size_dp;
    const float gap = AiDotModel::EdgeGapDp(side);
    SCOPED_TRACE(size);
    // At rest: its gap to the edge grows with it.
    EXPECT_FLOAT_EQ(AiDotModel::Rest(AiDotPlace{false, 500}, area, false).x_dp, 402.f - side - gap);
    EXPECT_FLOAT_EQ(AiDotModel::Rest(AiDotPlace{true, 500}, area, false).x_dp, gap);
    // Tucked: half of this size past the edge.
    EXPECT_FLOAT_EQ(AiDotModel::Rest(AiDotPlace{false, 500}, area, true).x_dp, 402.f - side / 2.f);
    EXPECT_FLOAT_EQ(AiDotModel::Rest(AiDotPlace{true, 500}, area, true).x_dp, -side / 2.f);
    // The lowest rest keeps the whole dot above the tab bar.
    EXPECT_FLOAT_EQ(AiDotModel::Rest(AiDotPlace{true, 1000}, area, false).y_dp, 874.f - 34.f - 57.f - side);
    // Which side it sticks to is decided by its own middle.
    EXPECT_FALSE(AiDotModel::Snap({201.f - side / 2.f + 1.f, 300.f}, area).left);
    EXPECT_TRUE(AiDotModel::Snap({201.f - side / 2.f - 1.f, 300.f}, area).left);
    // Dragged far off, the whole dot stays on the screen, inside the safe area.
    AiDotModel model;
    model.Show(0);
    const AiDotPoint rest = AiDotModel::Rest(AiDotPlace{}, area, false);
    model.Press({rest.x_dp, rest.y_dp}, rest);
    const auto offset = model.Move({rest.x_dp + 500.f, rest.y_dp + 2000.f}, area);
    ASSERT_TRUE(offset.has_value());
    EXPECT_FLOAT_EQ(rest.x_dp + offset->x_dp, 402.f - side);
    EXPECT_FLOAT_EQ(rest.y_dp + offset->y_dp, 874.f - 34.f - side);
  }
}

TEST(AiDotModelTest, TheBandRunsFromUnderTheNavBarToAboveTheTabBar) {
  const AiDotPoint top = AiDotModel::Rest(AiDotPlace{true, 0}, kArea, false);
  const AiDotPoint bottom = AiDotModel::Rest(AiDotPlace{true, 1000}, kArea, false);
  EXPECT_FLOAT_EQ(top.y_dp, 62.f + 44.f);
  EXPECT_FLOAT_EQ(bottom.y_dp, 874.f - 34.f - 57.f - kSize);
  EXPECT_FLOAT_EQ(top.x_dp, kGap);
}

TEST(AiDotModelTest, TuckedHalfOfItIsPastTheEdge) {
  EXPECT_FLOAT_EQ(AiDotModel::Rest(AiDotPlace{false, 500}, kArea, true).x_dp, 402.f - kSize / 2.f);
  EXPECT_FLOAT_EQ(AiDotModel::Rest(AiDotPlace{true, 500}, kArea, true).x_dp, -kSize / 2.f);
}

TEST(AiDotModelTest, ADragLetGoSnapsToTheNearerSideAndKeepsItsHeight) {
  AiDotModel model;
  model.Show(0);
  const AiDotPoint rest = AiDotModel::Rest(AiDotPlace{}, kArea, false);
  model.Press({rest.x_dp + 20.f, rest.y_dp + 20.f}, rest);
  // To the left third of the screen and a little up.
  const auto offset = model.Move({120.f, rest.y_dp - 80.f}, kArea);
  ASSERT_TRUE(offset.has_value());
  const AiDotModel::Release release = model.Up(kArea, 1000);
  EXPECT_FALSE(release.tap);
  ASSERT_TRUE(release.moved);
  EXPECT_TRUE(release.place.left);
  EXPECT_FLOAT_EQ(release.dropped.y_dp, rest.y_dp - 100.f);
  const AiDotPoint snapped = AiDotModel::Rest(release.place, kArea, false);
  EXPECT_FLOAT_EQ(snapped.x_dp, kGap);
  EXPECT_NEAR(snapped.y_dp, release.dropped.y_dp, 0.5f) << "only the side changes";
}

TEST(AiDotModelTest, PastTheMiddleItSticksToTheRight) {
  const AiDotPlace place = AiDotModel::Snap({402.f / 2.f - kSize / 2.f + 1.f, 300.f}, kArea);
  EXPECT_FALSE(place.left);
  EXPECT_TRUE(AiDotModel::Snap({402.f / 2.f - kSize / 2.f - 1.f, 300.f}, kArea).left);
}

TEST(AiDotModelTest, LetGoOverTheTabBarOrTheStatusBarItComesBackIntoTheBand) {
  EXPECT_EQ(AiDotModel::Snap({300.f, 860.f}, kArea).y_permille, 1000);
  EXPECT_EQ(AiDotModel::Snap({300.f, 0.f}, kArea).y_permille, 0);
}

TEST(AiDotModelTest, WhileDraggedItStaysOnTheScreen) {
  AiDotModel model;
  model.Show(0);
  const AiDotPoint rest = AiDotModel::Rest(AiDotPlace{}, kArea, false);
  model.Press({rest.x_dp, rest.y_dp}, rest);
  const auto offset = model.Move({rest.x_dp + 500.f, rest.y_dp + 2000.f}, kArea);
  ASSERT_TRUE(offset.has_value());
  EXPECT_FLOAT_EQ(rest.x_dp + offset->x_dp, 402.f - kSize);
  EXPECT_FLOAT_EQ(rest.y_dp + offset->y_dp, 874.f - 34.f - kSize);
}

TEST(AiDotModelTest, ASmallWobbleIsATapNotADrag) {
  AiDotModel model;
  model.Show(0);
  const AiDotPoint rest = AiDotModel::Rest(AiDotPlace{}, kArea, false);
  model.Press({rest.x_dp + 28.f, rest.y_dp + 28.f}, rest);
  EXPECT_FALSE(model.Move({rest.x_dp + 34.f, rest.y_dp + 32.f}, kArea).has_value());
  const AiDotModel::Release release = model.Up(kArea, 500);
  EXPECT_TRUE(release.tap);
  EXPECT_FALSE(release.moved);
}

TEST(AiDotModelTest, ADragNeverCountsAsATapEvenWhenItEndsWhereItBegan) {
  AiDotModel model;
  model.Show(0);
  const AiDotPoint rest = AiDotModel::Rest(AiDotPlace{}, kArea, false);
  model.Press({rest.x_dp, rest.y_dp}, rest);
  ASSERT_TRUE(model.Move({rest.x_dp - 60.f, rest.y_dp}, kArea).has_value());
  ASSERT_TRUE(model.Move({rest.x_dp, rest.y_dp}, kArea).has_value());
  const AiDotModel::Release release = model.Up(kArea, 500);
  EXPECT_FALSE(release.tap);
  EXPECT_TRUE(release.moved);
}

TEST(AiDotModelTest, TucksAfterFourAndAHalfSecondsAlone) {
  AiDotModel model;
  model.Show(0);
  EXPECT_FALSE(model.Advance(4499, true).tucked);
  EXPECT_FALSE(model.Tucked());
  EXPECT_TRUE(model.Advance(4500, true).tucked);
  EXPECT_TRUE(model.Tucked());
}

TEST(AiDotModelTest, APressBringsATuckedDotOutAndTheClockStartsAgainOnRelease) {
  AiDotModel model;
  model.Show(0);
  model.Advance(5000, true);
  ASSERT_TRUE(model.Tucked());
  const AiDotPoint rest = AiDotModel::Rest(AiDotPlace{}, kArea, false);
  model.Press({rest.x_dp, rest.y_dp}, rest);
  EXPECT_FALSE(model.Tucked());
  EXPECT_FALSE(model.Advance(20000, true).tucked) << "held: it does not tuck under the finger";
  model.Up(kArea, 20000);
  EXPECT_FALSE(model.Advance(24499, true).tucked);
  EXPECT_TRUE(model.Advance(24500, true).tucked);
}

TEST(AiDotModelTest, IdleMovesComeEveryFiveToTenSecondsAndTakeTurns) {
  for (int n = 0; n < 50; ++n) {
    EXPECT_GE(AiDotModel::IdleDelayMs(n), 5000);
    EXPECT_LE(AiDotModel::IdleDelayMs(n), 10000);
  }
  EXPECT_NE(AiDotModel::IdleDelayMs(0), AiDotModel::IdleDelayMs(1)) << "not a metronome";
  EXPECT_EQ(AiDotModel::IdleAction(0, true), AiDotIdle::Sway);
  EXPECT_EQ(AiDotModel::IdleAction(1, true), AiDotIdle::Blink);
  EXPECT_EQ(AiDotModel::IdleAction(1, false), AiDotIdle::Sway) << "the plain look has no eyes to blink";
}

TEST(AiDotModelTest, AnIdleMoveRunsOnlyAsLongAsItsKeyframes) {
  AiDotModel model;
  model.Show(0);
  const int64_t first = AiDotModel::IdleDelayMs(0);
  EXPECT_EQ(model.MsUntilNext(0), 4500) << "the tuck comes first";
  model.Advance(4500, true);
  EXPECT_EQ(model.MsUntilNext(4500), first - 4500);
  EXPECT_TRUE(model.Advance(first, true).idle);
  EXPECT_EQ(model.Idle(), AiDotIdle::Sway);
  EXPECT_EQ(model.MsUntilNext(first), AiDotModel::kSwayMs + AiDotModel::kIdleTailMs);
  EXPECT_TRUE(model.Advance(first + AiDotModel::kSwayMs + AiDotModel::kIdleTailMs, true).idle);
  EXPECT_EQ(model.Idle(), AiDotIdle::None);
  const auto next = model.MsUntilNext(first + AiDotModel::kSwayMs + AiDotModel::kIdleTailMs);
  ASSERT_TRUE(next.has_value());
  EXPECT_GE(*next, AiDotModel::kIdleMinMs) << "between moves nothing is due for at least 5 s";
}

TEST(AiDotModelTest, HiddenNothingIsDueSoTheAppCanSleep) {
  AiDotModel model;
  EXPECT_FALSE(model.MsUntilNext(0).has_value()) << "never shown";
  model.Show(0);
  ASSERT_TRUE(model.MsUntilNext(0).has_value());
  model.Hide();
  EXPECT_FALSE(model.MsUntilNext(100).has_value());
  EXPECT_FALSE(model.Advance(60000, true).tucked);
  EXPECT_FALSE(model.Advance(60000, true).idle);
}

TEST(AiDotModelTest, ShownAgainItComesOutAtItsEdge) {
  AiDotModel model;
  model.Show(0);
  model.Advance(5000, true);
  ASSERT_TRUE(model.Tucked());
  model.Hide();
  model.Show(9000);
  EXPECT_FALSE(model.Tucked());
  EXPECT_EQ(model.MsUntilNext(9000), 4500);
}

TEST(AiDotModelTest, APressStopsAnIdleMove) {
  AiDotModel model;
  model.Show(0);
  model.Advance(AiDotModel::IdleDelayMs(0), true);
  ASSERT_EQ(model.Idle(), AiDotIdle::Sway);
  const AiDotPoint rest = AiDotModel::Rest(AiDotPlace{}, kArea, false);
  model.Press({rest.x_dp, rest.y_dp}, rest);
  EXPECT_EQ(model.Idle(), AiDotIdle::None);
  EXPECT_FALSE(model.MsUntilNext(AiDotModel::IdleDelayMs(0)).has_value()) << "nothing runs while held";
}

// The idle moves are the real components.rcss keyframes: while one runs the context asks for frames, once it has run
// (the class still set) it asks for none, so a dot left alone costs no frames between its moves.
class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class FakeClock : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return now; }
  double now = 100.0;
};

class AiDotKeyframesTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&render);
    ui::SetSystemInterface(&clock);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("ai-dot-keyframes-test", {400, 800});
    ASSERT_NE(context, nullptr);
    const std::string themes = std::string(PP_BROWSER_ASSETS_DIR) + "/themes/";
    const std::string rml =
        "<rml><head><link type=\"text/rcss\" href=\"components.rcss\"/></head><body>"
        "<div class=\"shell-ai-dot-mount\"><div id=\"dot\" class=\"ai-dot ai-dot--shown\">"
        "<div id=\"figure\" class=\"ai-dot-figure\"><div id=\"eyes\" class=\"ai-dot-layer ai-dot-eyes-front\"></div>"
        "</div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml, themes + "ai_dot_keyframes_test.rml");
    ASSERT_NE(document, nullptr);
    document->Show();
    Frame(0.0);
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("ai-dot-keyframes-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
  }
  /** One app frame `seconds` after the last; true when the context asks for the next one at once. */
  bool Frame(const double seconds) {
    clock.now += seconds;
    context->Update();
    return context->GetNextUpdateDelay() == 0.0;
  }
  bool Idle() { return std::isinf(context->GetNextUpdateDelay()); }
  /** 60 Hz frames for as long as the context asks for them (RmlUi moves an animation at most 0.1 s a frame). */
  int FramesUntilIdle() {
    int frames = 0;
    while (Frame(kFrame) && frames < 1000) {
      ++frames;
    }
    return frames;
  }
  static constexpr double kFrame = 1.0 / 60.0;

  NullRender render;
  FakeClock clock;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
};

TEST_F(AiDotKeyframesTest, ALoneDotAsksForNoFrames) {
  EXPECT_FALSE(Frame(0.0));
  EXPECT_TRUE(Idle());
}

TEST_F(AiDotKeyframesTest, ASwayAsksForFramesOnlyWhileItRuns) {
  document->GetElementById("dot")->SetClass("ai-dot--sway", true);
  const int frames = FramesUntilIdle();
  // 60 Hz frames for its 1.2 s and not one more, the class still set (ShellAiDot drops it kIdleTailMs later).
  EXPECT_GE(frames, AiDotModel::kSwayMs * 60 / 1000 - 1);
  EXPECT_LE(frames, AiDotModel::kSwayMs * 60 / 1000 + 3);
  EXPECT_FALSE(Frame(kFrame));
  EXPECT_TRUE(Idle()) << "a finished sway asks for nothing more";
  document->GetElementById("dot")->SetClass("ai-dot--sway", false);
  EXPECT_FALSE(Frame(kFrame));
  EXPECT_TRUE(Idle());
}

TEST_F(AiDotKeyframesTest, ABlinkAsksForFramesOnlyWhileItRuns) {
  document->GetElementById("dot")->SetClass("ai-dot--blink", true);
  const int frames = FramesUntilIdle();
  EXPECT_GE(frames, AiDotModel::kBlinkMs * 60 / 1000 - 1) << "the eyes animate";
  EXPECT_LE(frames, AiDotModel::kBlinkMs * 60 / 1000 + 3);
  EXPECT_TRUE(Idle());
}

TEST_F(AiDotKeyframesTest, TheCalmSwayIsAsLongAsTheOther) {
  ui::Element* dot = document->GetElementById("dot");
  dot->SetClass("ai-dot--calm", true);
  dot->SetClass("ai-dot--sway", true);
  const int frames = FramesUntilIdle();
  EXPECT_GE(frames, AiDotModel::kSwayMs * 60 / 1000 - 1);
  EXPECT_LE(frames, AiDotModel::kSwayMs * 60 / 1000 + 3);
}

} // namespace
} // namespace pbr
