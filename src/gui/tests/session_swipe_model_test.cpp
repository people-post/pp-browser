#include "gui/shell/SessionSwipeModel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Kind = SessionSwipeOutcome::Kind;

constexpr float kRowWidthDp = 400.f;

// One drag on `row` from (100,100) moving by (dx,dy) px at dpr 1; returns the outcome of the release.
SessionSwipeOutcome Drag(SessionSwipeModel& model, const std::string& row, int dx, int dy) {
  model.Press(row, 100, 100, kRowWidthDp);
  model.Update(100 + dx / 2, 100 + dy / 2, 1.f);
  model.Update(100 + dx, 100 + dy, 1.f);
  return model.Release();
}

TEST(SessionSwipeModelTest, VerticalDragNeverOpens) {
  SessionSwipeModel model;
  model.Press("a", 100, 100, kRowWidthDp);
  EXPECT_FALSE(model.Update(104, 140, 1.f).claimed);
  EXPECT_FALSE(model.Update(180, 220, 1.f).claimed) << "the axis stays locked to vertical";
  const SessionSwipeOutcome out = model.Release();
  EXPECT_EQ(out.kind, Kind::None);
  EXPECT_FALSE(out.swallow_click);
  EXPECT_TRUE(model.OpenRow().empty());
}

TEST(SessionSwipeModelTest, SmallHorizontalMoveStaysClosed) {
  SessionSwipeModel model;
  const SessionSwipeOutcome out = Drag(model, "a", -40, 0);
  EXPECT_EQ(out.kind, Kind::Close);
  EXPECT_TRUE(model.OpenRow().empty());
}

TEST(SessionSwipeModelTest, LeftPastThresholdOpensEnd) {
  SessionSwipeModel model;
  const SessionSwipeOutcome out = Drag(model, "a", -120, 5);
  EXPECT_EQ(out.kind, Kind::Open);
  EXPECT_EQ(out.side, SwipeSide::End);
  EXPECT_EQ(model.OpenRow(), "a");
  EXPECT_FLOAT_EQ(model.RestOffsetDp(SwipeSide::End), -222.f);
}

TEST(SessionSwipeModelTest, RightPastThresholdOpensStart) {
  SessionSwipeModel model;
  const SessionSwipeOutcome out = Drag(model, "a", 120, -5);
  EXPECT_EQ(out.kind, Kind::Open);
  EXPECT_EQ(out.side, SwipeSide::Start);
  EXPECT_FLOAT_EQ(model.RestOffsetDp(SwipeSide::Start), 148.f);
}

TEST(SessionSwipeModelTest, NoDragLengthRunsAnAction) {
  SessionSwipeModel model;
  for (const int dx : {-130, -260, -380, -900, 130, 260, 380, 900}) {
    const SessionSwipeOutcome out = Drag(model, "a", dx, 0);
    EXPECT_EQ(out.kind, Kind::Open) << "a long drag only reveals the buttons (dx " << dx << ")";
    EXPECT_EQ(model.OpenRow(), "a");
    model.CloseOpen();
  }
}

TEST(SessionSwipeModelTest, EndSideHoldsMoreArchiveDelete) {
  SessionSwipeModel model;
  EXPECT_FLOAT_EQ(model.OpenWidthDp(SwipeSide::End), 3 * 74.f);
  EXPECT_FLOAT_EQ(model.OpenWidthDp(SwipeSide::Start), 2 * 74.f);
  EXPECT_FLOAT_EQ(model.RestOffsetDp(SwipeSide::End), -222.f);
}

TEST(SessionSwipeModelTest, OffsetFollowsTheFingerAndIsClamped) {
  SessionSwipeModel model;
  model.Press("a", 100, 100, kRowWidthDp);
  model.Update(90, 100, 1.f);
  EXPECT_FLOAT_EQ(model.Update(40, 100, 1.f).offset_dp, -60.f);
  EXPECT_FLOAT_EQ(model.Update(-900, 100, 1.f).offset_dp, -222.f) << "never slides past the buttons";
}

TEST(SessionSwipeModelTest, OpeningAnotherRowClosesTheFirst) {
  SessionSwipeModel model;
  ASSERT_EQ(Drag(model, "a", -120, 0).kind, Kind::Open);
  const std::string closed = model.Press("b", 100, 100, kRowWidthDp);
  EXPECT_EQ(closed, "a");
  EXPECT_TRUE(model.OpenRow().empty());
  model.Update(60, 100, 1.f);
  model.Update(-20, 100, 1.f);
  EXPECT_EQ(model.Release().kind, Kind::Open);
  EXPECT_EQ(model.OpenRow(), "b");
}

TEST(SessionSwipeModelTest, TapOnAnotherRowOnlyClosesAndIsSwallowed) {
  SessionSwipeModel model;
  ASSERT_EQ(Drag(model, "a", -120, 0).kind, Kind::Open);
  EXPECT_EQ(model.Press("b", 100, 100, kRowWidthDp), "a");
  const SessionSwipeOutcome out = model.Release();
  EXPECT_EQ(out.kind, Kind::Close);
  EXPECT_TRUE(out.swallow_click);
}

TEST(SessionSwipeModelTest, TapOnOpenRowClosesIt) {
  SessionSwipeModel model;
  ASSERT_EQ(Drag(model, "a", -120, 0).kind, Kind::Open);
  EXPECT_TRUE(model.Press("a", 100, 100, kRowWidthDp).empty());
  const SessionSwipeOutcome out = model.Release();
  EXPECT_EQ(out.kind, Kind::Close);
  EXPECT_TRUE(out.swallow_click);
  EXPECT_TRUE(model.OpenRow().empty());
}

TEST(SessionSwipeModelTest, ClickAfterDragIsSwallowedButPlainTapIsNot) {
  SessionSwipeModel model;
  EXPECT_TRUE(Drag(model, "a", -30, 0).swallow_click);
  model.Press("b", 100, 100, kRowWidthDp);
  const SessionSwipeOutcome tap = model.Release();
  EXPECT_EQ(tap.kind, Kind::None);
  EXPECT_FALSE(tap.swallow_click) << "a tap on a closed row opens the chat as before";
}

TEST(SessionSwipeModelTest, DraggingAnOpenRowBackClosesIt) {
  SessionSwipeModel model;
  ASSERT_EQ(Drag(model, "a", -120, 0).kind, Kind::Open);
  const SessionSwipeOutcome out = Drag(model, "a", 120, 0);
  EXPECT_EQ(out.kind, Kind::Close);
  EXPECT_TRUE(model.OpenRow().empty());
}

TEST(SessionSwipeModelTest, CloseOpenReturnsTheOpenRow) {
  SessionSwipeModel model;
  ASSERT_EQ(Drag(model, "a", -120, 0).kind, Kind::Open);
  EXPECT_EQ(model.CloseOpen(), "a");
  EXPECT_TRUE(model.CloseOpen().empty());
}

} // namespace
} // namespace pbr
