// DEV/PLAN-desktop-drop-paste.md, C: the collector of dragged-in files (no SDL type), the position -> UI conversion,
// and SdlAppEvents::PreProcess feeding it (links stay links; a position belongs to its own drag).
#include "common/PbrCompat.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/AppEventHooks.h"
#include "foundation/platform/IncomingLink.h"
#include "foundation/platform/ui/DropCollector.h"
#include "foundation/platform/ui/SdlAppEvents.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(DropCollectorTest, FilesComeOutInOrderWithTheLastPosition) {
  DropCollector c;
  c.OnPosition(10.f, 20.f, 0);
  c.OnPosition(30.f, 40.f, 100);
  c.OnFile("/a/one.png");
  c.OnFile("/a/two.pdf");
  c.OnFile("/a/three.zip");
  const DropCollector::Completed done = c.OnComplete();
  EXPECT_FALSE(done.left);
  EXPECT_EQ(done.paths, (std::vector<std::string>{"/a/one.png", "/a/two.pdf", "/a/three.zip"}));
  ASSERT_TRUE(done.position.has_value());
  EXPECT_FLOAT_EQ(done.position->x, 30.f);
  EXPECT_FLOAT_EQ(done.position->y, 40.f);
}

TEST(DropCollectorTest, TheStringIsCopiedAtOnce) {
  DropCollector c;
  c.OnPosition(1.f, 1.f, 0);
  std::string transient = "/tmp/dies-at-the-next-poll.txt";
  c.OnFile(transient);
  transient.assign(transient.size(), 'x');
  EXPECT_EQ(c.OnComplete().paths.front(), "/tmp/dies-at-the-next-poll.txt");
}

TEST(DropCollectorTest, ACompleteWithNoFileIsALeave) {
  DropCollector c;
  c.OnPosition(5.f, 5.f, 0);
  c.OnText(); // text drops are ignored
  const DropCollector::Completed done = c.OnComplete();
  EXPECT_TRUE(done.left);
  EXPECT_TRUE(done.paths.empty());
  EXPECT_FALSE(c.HintVisible());
}

TEST(DropCollectorTest, TheHintHidesAfterAStillSecondButTheDragAndItsPositionStay) {
  DropCollector c;
  c.OnPosition(50.f, 60.f, 1000);
  EXPECT_TRUE(c.HintVisible());
  EXPECT_FALSE(c.Tick(1900));
  EXPECT_TRUE(c.HintVisible());
  EXPECT_FALSE(c.Tick(2000)) << "exactly one second is not yet more than one second";
  EXPECT_TRUE(c.Tick(2001));
  EXPECT_FALSE(c.HintVisible());
  EXPECT_FALSE(c.Tick(9000)) << "hidden once";
  c.OnFile("/a/x.png");
  const DropCollector::Completed done = c.OnComplete();
  ASSERT_TRUE(done.position.has_value());
  EXPECT_FLOAT_EQ(done.position->x, 50.f);
}

TEST(DropCollectorTest, ALaterPositionShowsTheHintAgain) {
  DropCollector c;
  c.OnPosition(1.f, 1.f, 0);
  ASSERT_TRUE(c.Tick(1500));
  c.OnPosition(2.f, 2.f, 1600);
  EXPECT_TRUE(c.HintVisible());
  EXPECT_FALSE(c.Tick(2500));
  EXPECT_TRUE(c.Tick(2700));
}

TEST(DropCollectorTest, AFifteenSecondHoldStillDropsWhereThePointerWas) {
  DropCollector c;
  c.OnPosition(70.f, 80.f, 0);
  EXPECT_TRUE(c.Tick(1500));
  EXPECT_FALSE(c.Tick(15000));
  c.OnFile("/a/x.png");
  const DropCollector::Completed done = c.OnComplete();
  ASSERT_TRUE(done.position.has_value()) << "no timer ever invalidates the position";
  EXPECT_FLOAT_EQ(done.position->x, 70.f);
  EXPECT_FLOAT_EQ(done.position->y, 80.f);
}

TEST(DropCollectorTest, TheWakeIsArmedForTheSecondAfterAPosition) {
  DropCollector c;
  EXPECT_FALSE(c.MsUntilHintHide(0).has_value());
  c.OnPosition(1.f, 1.f, 500);
  ASSERT_TRUE(c.MsUntilHintHide(500).has_value());
  EXPECT_EQ(*c.MsUntilHintHide(500), 1001) << "the first tick that is more than a second later";
  EXPECT_EQ(*c.MsUntilHintHide(1200), 301);
  EXPECT_EQ(*c.MsUntilHintHide(5000), 0) << "overdue: at once";
  ASSERT_TRUE(c.Tick(1501));
  EXPECT_FALSE(c.MsUntilHintHide(1501).has_value()) << "nothing pending once it is hidden";
}

TEST(DropCollectorTest, LosingFocusForgetsTheDrag) {
  DropCollector c;
  c.OnPosition(1.f, 1.f, 0);
  EXPECT_TRUE(c.OnFocusLost());
  EXPECT_FALSE(c.HintVisible());
  c.OnFile("/a/x.png");
  EXPECT_FALSE(c.OnComplete().position.has_value()) << "the position went with the focus";
  EXPECT_FALSE(c.OnFocusLost()) << "nothing was showing";
}

TEST(DropCollectorTest, FilesWithNoPositionEverCarryNoPosition) {
  DropCollector c; // the Windows WM_DROPFILES fallback: files and a complete, no hover
  c.OnFile("/a/x.png");
  const DropCollector::Completed done = c.OnComplete();
  EXPECT_FALSE(done.left);
  EXPECT_EQ(done.paths.size(), 1u);
  EXPECT_FALSE(done.position.has_value());
}

// The drag that never completed (a drop macOS rejects sends no FILE and no COMPLETE) leaves its position behind.
TEST(DropCollectorTest, ADragThatNeverCompletedDoesNotLendItsPositionToTheNext) {
  DropCollector c;
  c.OnPosition(900.f, 900.f, 0); // old drag, rejected: nothing follows
  c.OnBegin();                   // SDL sends BEGIN when it does see a new drag
  c.OnFile("/a/x.png");          // the new drag brought no position of its own
  EXPECT_FALSE(c.OnComplete().position.has_value());
}

TEST(DropCollectorTest, ANewDragsOwnPositionReplacesTheOldOne) {
  DropCollector c;
  c.OnPosition(900.f, 900.f, 0);
  c.OnPosition(10.f, 10.f, 5000); // the next drag's hover (no COMPLETE came for the old one)
  c.OnFile("/a/x.png");
  const DropCollector::Completed done = c.OnComplete();
  ASSERT_TRUE(done.position.has_value());
  EXPECT_FLOAT_EQ(done.position->x, 10.f);
}

TEST(DropCollectorTest, FilesOfAnUncompletedDragAreNotMixedIntoTheNext) {
  DropCollector c;
  c.OnPosition(1.f, 1.f, 0);
  c.OnFile("/old/a.png"); // its COMPLETE got lost
  c.OnPosition(2.f, 2.f, 3000);
  c.OnFile("/new/b.png");
  const DropCollector::Completed done = c.OnComplete();
  EXPECT_EQ(done.paths, (std::vector<std::string>{"/new/b.png"}));
  ASSERT_TRUE(done.position.has_value());
  EXPECT_FLOAT_EQ(done.position->x, 2.f);
}

TEST(DropCollectorTest, AfterACompleteTheNextDragStartsClean) {
  DropCollector c;
  c.OnPosition(1.f, 1.f, 0);
  c.OnFile("/a/x.png");
  c.OnComplete();
  c.OnFile("/b/y.png");
  const DropCollector::Completed done = c.OnComplete();
  EXPECT_EQ(done.paths, (std::vector<std::string>{"/b/y.png"}));
  EXPECT_FALSE(done.position.has_value());
}

TEST(DropPointToUiTest, UsesThePixelDensityTheMouseUses) {
  const DropUiPoint at1 = DropPointToUi({100.f, 50.f}, 1.f);
  EXPECT_EQ(at1.x, 100);
  EXPECT_EQ(at1.y, 50);
  const DropUiPoint at15 = DropPointToUi({100.7f, 50.5f}, 1.5f);
  EXPECT_EQ(at15.x, 151) << "truncated, as int(x * density) in the engine's mouse path";
  EXPECT_EQ(at15.y, 75);
  const DropUiPoint at2 = DropPointToUi({100.7f, 50.5f}, 2.f);
  EXPECT_EQ(at2.x, 201);
  EXPECT_EQ(at2.y, 101);
}

// SdlAppEvents::PreProcess: the events as SDL pushes them.
class SdlDropEventsTest : public testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(AppRuntimeConfig{});
    SdlAppEvents::ResetDropForTest();
    AppEventHooks hooks;
    hooks.on_drop_hover = [this](std::optional<DropUiPoint> p) { hovers.push_back(p); };
    hooks.on_files_dropped = [this](std::vector<std::string> paths, std::optional<DropUiPoint> p) {
      dropped.push_back({std::move(paths), p});
    };
    SetAppEventHooks(std::move(hooks));
    IncomingLink::SetHandler([this](std::string link) { links.push_back(std::move(link)); });
  }
  void TearDown() override {
    SetAppEventHooks({});
    IncomingLink::SetHandler({});
    SdlAppEvents::ResetDropForTest();
    AppRuntime::Shutdown();
  }

  bool Feed(const SDL_EventType type, const char* data = nullptr, const float x = 0.f, const float y = 0.f) {
    SDL_Event event;
    SDL_zero(event);
    event.type = type;
    event.drop.data = data;
    event.drop.x = x;
    event.drop.y = y;
    bool propagate = true;
    return SdlAppEvents::PreProcess(nullptr, event, propagate);
  }

  struct Drop {
    std::vector<std::string> paths;
    std::optional<DropUiPoint> position;
  };
  std::vector<std::optional<DropUiPoint>> hovers;
  std::vector<Drop> dropped;
  std::vector<std::string> links;
};

TEST_F(SdlDropEventsTest, AFileDragIsHoveredThenDeliveredAtTheLastPosition) {
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_BEGIN) == false);
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_POSITION, nullptr, 120.f, 80.f));
  ASSERT_EQ(hovers.size(), 1u);
  ASSERT_TRUE(hovers[0].has_value());
  EXPECT_EQ(hovers[0]->x, 120) << "no window in a test: density 1";
  EXPECT_EQ(hovers[0]->y, 80);
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_POSITION, nullptr, 130.f, 90.f));
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_FILE, "/a/one.png", 130.f, 90.f));
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_FILE, "/a/two.pdf", 130.f, 90.f));
  EXPECT_TRUE(dropped.empty()) << "nothing is delivered before the complete";
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_COMPLETE));
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].paths, (std::vector<std::string>{"/a/one.png", "/a/two.pdf"}));
  ASSERT_TRUE(dropped[0].position.has_value());
  EXPECT_EQ(dropped[0].position->x, 130);
  ASSERT_FALSE(hovers.back().has_value()) << "the hint hides on the complete";
}

TEST_F(SdlDropEventsTest, ALeaveHidesTheHintAndDeliversNothing) {
  Feed(SDL_EVENT_DROP_POSITION, nullptr, 5.f, 5.f);
  Feed(SDL_EVENT_DROP_COMPLETE);
  EXPECT_TRUE(dropped.empty());
  EXPECT_FALSE(hovers.back().has_value());
}

TEST_F(SdlDropEventsTest, LinkDataIsALinkAndNeverEntersTheCollector) {
  Feed(SDL_EVENT_DROP_POSITION, nullptr, 5.f, 5.f);
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_FILE, "ppbrowser://add?x=1", 5.f, 5.f));
  EXPECT_TRUE(Feed(SDL_EVENT_DROP_FILE, "/a/real.png", 5.f, 5.f));
  Feed(SDL_EVENT_DROP_COMPLETE);
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_EQ(dropped[0].paths, (std::vector<std::string>{"/a/real.png"})) << "the link is not a dropped file";
  ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
  EXPECT_EQ(links, (std::vector<std::string>{"ppbrowser://add?x=1"})) << "it went the link way, as before";
}

TEST_F(SdlDropEventsTest, ALoneLinkDropDeliversNoFiles) {
  Feed(SDL_EVENT_DROP_BEGIN);
  Feed(SDL_EVENT_DROP_FILE, "ppbrowser://open", 0.f, 0.f);
  Feed(SDL_EVENT_DROP_COMPLETE);
  EXPECT_TRUE(dropped.empty()) << "a complete with only a link in it is a leave";
  ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
  EXPECT_EQ(links.size(), 1u);
}

TEST_F(SdlDropEventsTest, FocusLossHidesTheHintAndForgetsTheDrag) {
  Feed(SDL_EVENT_DROP_POSITION, nullptr, 5.f, 5.f);
  Feed(SDL_EVENT_WINDOW_FOCUS_LOST);
  EXPECT_FALSE(hovers.back().has_value());
  Feed(SDL_EVENT_DROP_FILE, "/a/x.png", 0.f, 0.f);
  Feed(SDL_EVENT_DROP_COMPLETE);
  ASSERT_EQ(dropped.size(), 1u);
  EXPECT_FALSE(dropped[0].position.has_value()) << "SDL's own x/y of a FILE (0, 0) is never taken for a position";
}

TEST_F(SdlDropEventsTest, WithoutTheHooksTheEventsAreLeftAlone) {
  SetAppEventHooks({});
  EXPECT_FALSE(Feed(SDL_EVENT_DROP_POSITION, nullptr, 5.f, 5.f));
  EXPECT_FALSE(Feed(SDL_EVENT_DROP_FILE, "/a/x.png", 5.f, 5.f));
  EXPECT_FALSE(Feed(SDL_EVENT_DROP_COMPLETE));
}

} // namespace
} // namespace pbr
