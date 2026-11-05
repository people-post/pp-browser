// Kenneth 2026-11-03 (iPhone): on the Chats list he swiped a row open and tapped Delete; the chat opened and no delete
// confirm came. A real context with the swipe gesture attached: the revealed buttons must get the tap.
#include "gui/shell/SessionSwipeGesture.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/EventListener.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

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

/** Counts clicks that reach the row itself (in the app: select_thread, which opens the chat). */
class ClickCounter : public ui::EventListener {
public:
  void ProcessEvent(ui::Event&) override { ++clicks; }
  int clicks = 0;
};

// The Chats list geometry of the phone (components.rcss): a 400dp list that hides what overflows sideways, 60dp rows,
// three 74dp buttons just right of the row (More, Archive, Delete) and two just left of it (Pin, Read).
constexpr const char* kListRml = R"(<rml><head><style>
body { display: block; width: 400px; height: 800px; margin: 0; }
div { display: block; }
#sidebar-sessions { width: 400px; height: 600px; overflow-x: hidden; }
.sidebar-session { position: relative; height: 60px; }
.session-swipe { display: none; }
.sidebar-session.session-swipe-shown .session-swipe { display: flex; flex-direction: row; position: absolute; top: 0; bottom: 0; }
.session-swipe--end { left: 100%; }
.session-swipe--start { right: 100%; }
.session-swipe-btn { width: 74px; height: 60px; }
#home-ai-sessions { width: 400px; height: 100px; overflow-x: hidden; }
</style></head><body><div id="page"><div id="sidebar-sessions">
<div class="sidebar-session" id="row" session-id="t1">
  <div class="session-swipe session-swipe--end">
    <div class="session-swipe-btn" swipe-action="more"></div>
    <div class="session-swipe-btn" swipe-action="archive"></div>
    <div class="session-swipe-btn" swipe-action="delete"></div>
  </div>
  <div class="session-swipe session-swipe--start">
    <div class="session-swipe-btn" swipe-action="pin"></div>
    <div class="session-swipe-btn" swipe-action="read"></div>
  </div>
</div>
</div>
<div id="home-ai-sessions">
<div class="sidebar-session" id="ai_row" session-id="ai1">
  <div class="session-swipe session-swipe--end">
    <div class="session-swipe-btn" swipe-action="more"></div>
    <div class="session-swipe-btn" swipe-action="archive"></div>
    <div class="session-swipe-btn" swipe-action="delete"></div>
  </div>
  <div class="session-swipe session-swipe--start">
    <div class="session-swipe-btn" swipe-action="pin"></div>
    <div class="session-swipe-btn" swipe-action="rename"></div>
  </div>
</div>
</div>
<div class="sidebar-session" id="outside_row" session-id="x1"></div>
</div></body></rml>)";

class SessionSwipeHitTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("session-swipe-hit-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(kListRml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    row_ = document_->GetElementById("row");
    ASSERT_NE(row_, nullptr);
    row_->AddEventListener(ui::EventId::Click, &row_clicks_);
    gesture_.Attach(document_->GetElementById("page"), context_,
                    [this](const std::string& thread_id, const SessionSwipeAction action) {
                      actions_.push_back(thread_id + ":" + std::to_string(static_cast<int>(action)));
                    });
  }

  void TearDown() override {
    gesture_.Detach();
    if (row_) {
      row_->RemoveEventListener(ui::EventId::Click, &row_clicks_);
    }
    if (context_) {
      ui::RemoveContext("session-swipe-hit-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void Drag(const int from_x, const int to_x, const int y = 30) {
    const int kY = y;
    context_->ProcessMouseMove(from_x, kY, 0);
    context_->ProcessMouseButtonDown(0, 0);
    const int steps = 10;
    for (int i = 1; i <= steps; ++i) {
      context_->ProcessMouseMove(from_x + (to_x - from_x) * i / steps, kY, 0);
      context_->Update();
    }
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  void Tap(const int x, const int y = 30) {
    context_->ProcessMouseMove(x, y, 0);
    context_->ProcessMouseButtonDown(0, 0);
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  static std::string Action(const SessionSwipeAction action) { return "t1:" + std::to_string(static_cast<int>(action)); }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Element* row_ = nullptr;
  ClickCounter row_clicks_;
  SessionSwipeGesture gesture_;
  std::vector<std::string> actions_;
};

// Open by dragging left exactly the buttons' width (222), so the row rests where the drag left it.
TEST_F(SessionSwipeHitTest, EachEndButtonGetsItsTapAndTheChatDoesNotOpen) {
  const struct {
    int x;
    SessionSwipeAction action;
  } taps[] = {{215, SessionSwipeAction::More}, {289, SessionSwipeAction::Archive}, {363, SessionSwipeAction::Delete}};
  for (const auto& tap : taps) {
    actions_.clear();
    row_clicks_.clicks = 0;
    Drag(380, 158);
    ASSERT_TRUE(row_->IsClassSet("session-swipe-shown"));
    Tap(tap.x);
    EXPECT_EQ(actions_, std::vector<std::string>{Action(tap.action)}) << tap.x;
    EXPECT_EQ(row_clicks_.clicks, 0) << "the tap must not reach the row (it would open the chat), x=" << tap.x;
  }
}

TEST_F(SessionSwipeHitTest, EachStartButtonGetsItsTap) {
  const struct {
    int x;
    SessionSwipeAction action;
  } taps[] = {{37, SessionSwipeAction::Pin}, {111, SessionSwipeAction::Read}};
  for (const auto& tap : taps) {
    actions_.clear();
    row_clicks_.clicks = 0;
    Drag(20, 168);
    ASSERT_TRUE(row_->IsClassSet("session-swipe-shown"));
    Tap(tap.x);
    EXPECT_EQ(actions_, std::vector<std::string>{Action(tap.action)}) << tap.x;
    EXPECT_EQ(row_clicks_.clicks, 0) << tap.x;
  }
}

// The PP助手 page's AI list (#home-ai-sessions, below the 600px chats list): its rows swipe the same way, and the start
// side's second button is Rename.
TEST_F(SessionSwipeHitTest, AiListRowsSwipeAndTheStartSideOffersPinAndRename) {
  constexpr int kAiY = 630;
  ui::Element* ai_row = document_->GetElementById("ai_row");
  ASSERT_NE(ai_row, nullptr);
  const struct {
    int x;
    SessionSwipeAction action;
  } taps[] = {{37, SessionSwipeAction::Pin}, {111, SessionSwipeAction::Rename}};
  for (const auto& tap : taps) {
    actions_.clear();
    Drag(20, 168, kAiY);
    ASSERT_TRUE(ai_row->IsClassSet("session-swipe-shown"));
    Tap(tap.x, kAiY);
    EXPECT_EQ(actions_, std::vector<std::string>{"ai1:" + std::to_string(static_cast<int>(tap.action))}) << tap.x;
  }
  actions_.clear();
  Drag(380, 158, kAiY);
  ASSERT_TRUE(ai_row->IsClassSet("session-swipe-shown"));
  Tap(363, kAiY);
  EXPECT_EQ(actions_, std::vector<std::string>{"ai1:" + std::to_string(static_cast<int>(SessionSwipeAction::Delete))});
}

TEST_F(SessionSwipeHitTest, ARowOutsideBothListsDoesNotSwipe) {
  ui::Element* outside = document_->GetElementById("outside_row");
  ASSERT_NE(outside, nullptr);
  Drag(380, 158, 700);
  EXPECT_FALSE(outside->IsClassSet("session-swipe-shown"));
}

TEST_F(SessionSwipeHitTest, TappingTheVisiblePartOfAnOpenRowClosesItWithoutOpeningTheChat) {
  Drag(380, 158);
  ASSERT_TRUE(row_->IsClassSet("session-swipe-shown"));
  Tap(80);
  EXPECT_TRUE(actions_.empty());
  EXPECT_EQ(row_clicks_.clicks, 0);
  EXPECT_FALSE(row_->IsClassSet("session-swipe-shown"));
  // Closed again, a tap on the row is a normal tap (it opens the chat).
  Tap(80);
  EXPECT_EQ(row_clicks_.clicks, 1);
}

} // namespace
} // namespace pbr
