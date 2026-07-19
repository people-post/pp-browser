#include "feature/ui/FlowCoordinator.h"

#include <gtest/gtest.h>

TEST(FlowCoordinatorTest, StepBackKeepsFlowActive) {
  int step = 1;
  pbr::FlowCoordinator::Instance().BeginModal(42, [&]() {
    step = 0;
    return true;
  }, []() {});
  EXPECT_TRUE(pbr::FlowCoordinator::Instance().HandleDismiss());
  EXPECT_EQ(step, 0);
  EXPECT_TRUE(pbr::FlowCoordinator::Instance().IsActive());
  pbr::FlowCoordinator::Instance().EndModal();
}

TEST(FlowCoordinatorTest, CancelEndsFlow) {
  bool cancelled = false;
  pbr::FlowCoordinator::Instance().BeginModal(42, []() { return false; }, [&]() { cancelled = true; });
  EXPECT_TRUE(pbr::FlowCoordinator::Instance().HandleDismiss());
  EXPECT_TRUE(cancelled);
  EXPECT_FALSE(pbr::FlowCoordinator::Instance().IsActive());
}

TEST(FlowCoordinatorTest, LayerClosingInvokesCancel) {
  bool cancelled = false;
  pbr::FlowCoordinator::Instance().BeginModal(7, []() { return true; }, [&]() { cancelled = true; });
  pbr::FlowCoordinator::Instance().NotifyLayerClosing(7);
  EXPECT_TRUE(cancelled);
  EXPECT_FALSE(pbr::FlowCoordinator::Instance().IsActive());
}
