#include "gui/CallSpotlight.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallRosterParticipantState Row(const char* stream, bool is_local = false) {
  CallRosterParticipantState row;
  row.stream_id = stream;
  row.is_local = is_local;
  return row;
}

// V059: a 1:1 call's Immersive spotlights the peer; a group's shows the grid until someone is tapped.
TEST(CallSpotlightTest, AOneToOneCallSpotlightsThePeerAndAGroupShowsTheGrid) {
  const std::vector<CallRosterParticipantState> pair = {Row("11", true), Row("22")};
  EXPECT_EQ(ResolveCallSpotlight(pair, /*group=*/false, {}), "22");
  const std::vector<CallRosterParticipantState> group = {Row("11", true), Row("22"), Row("33")};
  EXPECT_EQ(ResolveCallSpotlight(group, /*group=*/true, {}), "");
}

// The user's tap wins while that person is in the call, the self-view included; when they leave, the default.
TEST(CallSpotlightTest, TheTappedTileIsSpotlitWhileThatPersonIsInTheCall) {
  std::vector<CallRosterParticipantState> group = {Row("11", true), Row("22"), Row("33")};
  EXPECT_EQ(ResolveCallSpotlight(group, true, {.stream = "33"}), "33");
  EXPECT_EQ(ResolveCallSpotlight(group, true, {.stream = "11"}), "11") << "the self-view can be spotlit";
  group.pop_back();
  EXPECT_EQ(ResolveCallSpotlight(group, true, {.stream = "33"}), "") << "they left: back to the grid";
  const std::vector<CallRosterParticipantState> pair = {Row("11", true), Row("22")};
  EXPECT_EQ(ResolveCallSpotlight(pair, false, {.stream = "11"}), "11") << "1:1: the user swapped to the self-view";
}

// A group call spotlights its active speaker until someone taps; "back to grid" holds over it too.
TEST(CallSpotlightTest, AGroupSpotlightsItsActiveSpeakerUntilSomeoneChooses) {
  const std::vector<CallRosterParticipantState> group = {Row("11", true), Row("22"), Row("33")};
  EXPECT_EQ(ResolveCallSpotlight(group, true, {}, "33"), "33");
  EXPECT_EQ(ResolveCallSpotlight(group, true, {}, "44"), "") << "a speaker no longer in the call";
  EXPECT_EQ(ResolveCallSpotlight(group, true, {.stream = "22"}, "33"), "22") << "a pin wins";
  EXPECT_EQ(ResolveCallSpotlight(group, true, {.grid = true}, "33"), "") << "the grid wins";
}

// "Back to grid" stays: a 1:1 call does not spotlight the peer again by itself.
TEST(CallSpotlightTest, BackToGridStaysUntilTheNextTap) {
  const std::vector<CallRosterParticipantState> pair = {Row("11", true), Row("22")};
  EXPECT_EQ(ResolveCallSpotlight(pair, false, {.grid = true}), "");
  EXPECT_EQ(ResolveCallSpotlight(pair, false, {.stream = "22", .grid = true}), "22") << "a tap after the grid";
}

} // namespace
} // namespace pbr
