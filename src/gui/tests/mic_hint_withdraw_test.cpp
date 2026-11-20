#include "gui/UserFeedback.h"
#include "gui/shell/ShellFeedback.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {

// A pill that replaced ours is not taken down with it.
TEST(MicHintWithdrawTest, OnlyTheToastWithTheMessageIsTakenDown) {
  ShellState state{};
  ShellFeedback::ShowToast(state, "They may not hear you.", ToastDuration::Long, 1000.f);
  ShellFeedback::ShowToast(state, "Something else", ToastDuration::Long, 1000.f);
  ASSERT_EQ(state.toasts.size(), 2U);
  EXPECT_TRUE(ShellFeedback::DismissToastWithMessage(state, "They may not hear you."));
  ASSERT_EQ(state.toasts.size(), 1U);
  EXPECT_STREQ(state.toasts.front().message.c_str(), "Something else");
  EXPECT_FALSE(ShellFeedback::DismissToastWithMessage(state, "They may not hear you.")) << "already gone";
  EXPECT_EQ(state.toasts.size(), 1U);
}

TEST(MicHintWithdrawTest, OnAPhoneAPillThatReplacedOursStays) {
  ShellState state{};
  state.layout_mode = LayoutMode::Compact; // one pill at a time: the new one replaces the old
  ShellFeedback::ShowToast(state, "They may not hear you.", ToastDuration::Long, 1000.f);
  ShellFeedback::ShowToast(state, "Message sent", ToastDuration::Short, 1500.f);
  ASSERT_EQ(state.toasts.size(), 1U);
  EXPECT_FALSE(ShellFeedback::DismissToastWithMessage(state, "They may not hear you."));
  EXPECT_EQ(state.toasts.size(), 1U) << "the other pill is not removed";
}

TEST(MicHintWithdrawTest, TheUserFeedbackPortPassesTheMessageAndNothingElse) {
  std::vector<std::string> dismissed;
  int blind_dismisses = 0;
  ShellFeedbackPorts ports;
  ports.dismiss_toast_with_message = [&dismissed](const std::string& message) { dismissed.push_back(message); };
  ports.dismiss_toast = [&blind_dismisses]() { ++blind_dismisses; };
  UserFeedback::BindPorts(std::move(ports));
  UserFeedback::DismissToastWithMessage("our pill");
  UserFeedback::BindPorts({});
  ASSERT_EQ(dismissed.size(), 1U);
  EXPECT_EQ(dismissed.front(), "our pill");
  EXPECT_EQ(blind_dismisses, 0);
}

} // namespace pbr
