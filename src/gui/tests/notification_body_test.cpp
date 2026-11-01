#include "gui/chat/NotificationBody.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(NotificationBodyTest, ShowsTheMessageWhenPreviewIsOn) {
  EXPECT_EQ(NotificationBodyFor(true, "see you at 8", "New message"), "see you at 8");
}

TEST(NotificationBodyTest, ShowsTheNeutralLineWhenPreviewIsOff) {
  EXPECT_EQ(NotificationBodyFor(false, "see you at 8", "New message"), "New message");
}

} // namespace
} // namespace pbr
