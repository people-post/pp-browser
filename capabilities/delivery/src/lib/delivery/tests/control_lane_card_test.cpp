// When a control-lane envelope carries our account card (~13 KB + 4 KB per device): only when the peer may not
// hold it. Every lane message used to carry it, and the relay filled up with them.
#include "lib/delivery/ControlLaneEnvelope.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(ControlLaneCardTest, ThePeerNeverSentTheCardGetsIt) {
  EXPECT_TRUE(ControlLaneCarriesCard(false, false, std::nullopt, 100));
}

TEST(ControlLaneCardTest, TheSameCardIsNotSentTwice) {
  EXPECT_FALSE(ControlLaneCarriesCard(false, false, 100, 100));
}

TEST(ControlLaneCardTest, ANewerCardGoesOutOnce) {
  EXPECT_TRUE(ControlLaneCarriesCard(false, false, 100, 200));
}

TEST(ControlLaneCardTest, AnOpenerOrAnAskCarriesItWhateverWasSent) {
  EXPECT_TRUE(ControlLaneCarriesCard(true, false, 100, 100)) << "a session opener: the peer derives the session";
  EXPECT_TRUE(ControlLaneCarriesCard(false, true, 100, 100)) << "the caller knows the peer may hold none of ours";
}

} // namespace
} // namespace pbr
