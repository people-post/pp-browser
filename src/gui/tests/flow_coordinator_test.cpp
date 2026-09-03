#include "gui/FlowCoordinator.h"

#include <gtest/gtest.h>

TEST(FlowCoordinatorTest, StepBackKeepsFlowActive) {
  pbr::FlowCoordinator flow;
  int step = 1;
  flow.BeginModal(42, [&]() {
    step = 0;
    return true;
  }, []() {});
  EXPECT_TRUE(flow.HandleDismiss());
  EXPECT_EQ(step, 0);
  EXPECT_TRUE(flow.IsActive());
  flow.EndModal();
}

TEST(FlowCoordinatorTest, CancelEndsFlow) {
  pbr::FlowCoordinator flow;
  bool cancelled = false;
  flow.BeginModal(42, []() { return false; }, [&]() { cancelled = true; });
  EXPECT_TRUE(flow.HandleDismiss());
  EXPECT_TRUE(cancelled);
  EXPECT_FALSE(flow.IsActive());
}

TEST(FlowCoordinatorTest, LayerClosingInvokesCancel) {
  pbr::FlowCoordinator flow;
  bool cancelled = false;
  flow.BeginModal(7, []() { return true; }, [&]() { cancelled = true; });
  flow.NotifyLayerClosing(7);
  EXPECT_TRUE(cancelled);
  EXPECT_FALSE(flow.IsActive());
}
