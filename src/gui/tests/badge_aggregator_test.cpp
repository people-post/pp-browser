#include "gui/BadgeAggregator.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace pbr {
namespace {

struct Harness {
  BadgeAggregator badges;
  std::optional<int> icon_source; // what the source reports for the icon
  int sessions = 0;
  std::vector<int> sent; // every call of the icon port

  Harness() {
    badges.BindSource([this]() {
      BadgeUnreadInputs inputs;
      inputs.sessions_unread = sessions;
      inputs.app_icon_count = icon_source;
      return inputs;
    });
    badges.BindAppBadge([this](int count) { sent.push_back(count); });
  }
};

TEST(BadgeAggregatorIconTest, MessagingNotInitializedNeverTouchesTheIcon) {
  Harness h; // icon_source stays empty
  h.badges.Refresh();
  h.badges.Refresh();
  EXPECT_TRUE(h.sent.empty());
}

TEST(BadgeAggregatorIconTest, FirstValueIsAlwaysSentEvenZero) {
  Harness h;
  h.icon_source = 0;
  h.badges.Refresh();
  ASSERT_EQ(h.sent.size(), 1U);
  EXPECT_EQ(h.sent[0], 0);
}

TEST(BadgeAggregatorIconTest, AnUnchangedNumberIsNotSentAgain) {
  Harness h;
  h.icon_source = 3;
  h.badges.Refresh();
  h.badges.Refresh();
  EXPECT_EQ(h.sent, (std::vector<int>{3}));
  h.icon_source = 2;
  h.badges.Refresh();
  EXPECT_EQ(h.sent, (std::vector<int>{3, 2}));
}

TEST(BadgeAggregatorIconTest, InvalidateSendsTheSameNumberOnce) {
  Harness h;
  h.icon_source = 3;
  h.badges.Refresh();
  h.badges.InvalidateAppBadge();
  h.badges.Refresh();
  h.badges.Refresh();
  EXPECT_EQ(h.sent, (std::vector<int>{3, 3}));
}

TEST(BadgeAggregatorIconTest, InvalidateWithNoValueSendsNothing) {
  Harness h;
  h.badges.InvalidateAppBadge();
  h.badges.Refresh();
  EXPECT_TRUE(h.sent.empty());
}

TEST(BadgeAggregatorIconTest, ClearSendsZeroAndZeroIsNotRepeated) {
  Harness h;
  h.icon_source = 4;
  h.badges.Refresh();
  h.badges.ClearAppBadge();
  EXPECT_EQ(h.sent, (std::vector<int>{4, 0}));
  h.icon_source = 0;
  h.badges.Refresh();
  EXPECT_EQ(h.sent, (std::vector<int>{4, 0})) << "the icon already shows 0";
}

TEST(BadgeAggregatorIconTest, AfterClearANonZeroTotalIsSentAgain) {
  Harness h;
  h.icon_source = 4;
  h.badges.Refresh();
  h.badges.ClearAppBadge();
  h.icon_source = 2; // the reset inbox found 2
  h.badges.Refresh();
  EXPECT_EQ(h.sent, (std::vector<int>{4, 0, 2}));
}

TEST(BadgeAggregatorIconTest, ClearWithoutAPortIsHarmless) {
  BadgeAggregator badges;
  badges.ClearAppBadge();
  badges.Refresh();
  SUCCEED();
}

TEST(BadgeAggregatorIconTest, TheNavBadgeStillFollowsTheSessionsCount) {
  Harness h;
  h.sessions = 5;
  h.icon_source = 9;
  h.badges.Refresh();
  EXPECT_EQ(h.badges.State().sessions_unread, 5);
}

TEST(AppIconBadgeCountTest, NotInitializedIsEmptyWhateverTheTotal) {
  EXPECT_FALSE(AppIconBadgeCount(false, 0).has_value());
  EXPECT_FALSE(AppIconBadgeCount(false, 7).has_value());
}

TEST(AppIconBadgeCountTest, InitializedIsTheRawTotalNeverNegative) {
  EXPECT_EQ(AppIconBadgeCount(true, 7), 7);
  EXPECT_EQ(AppIconBadgeCount(true, 0), 0);
  EXPECT_EQ(AppIconBadgeCount(true, -2), 0);
}

} // namespace
} // namespace pbr
