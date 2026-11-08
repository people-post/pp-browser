// When a control-lane envelope carries our account card (~13 KB + 4 KB per device): only when a device it addresses
// may not hold it. Every lane message used to carry it, and the relay filled up with them; then it went once per
// account, and a device linked to the account later never got it (#555).
#include "lib/delivery/ControlLaneEnvelope.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(ControlLaneCardTest, ThePeerNeverSentTheCardGetsIt) {
  EXPECT_TRUE(ControlLaneCarriesCard(false, false, {std::nullopt}, 100));
  EXPECT_TRUE(ControlLaneCarriesCard(false, false, {}, 100)) << "nobody addressed known: carry it";
}

TEST(ControlLaneCardTest, TheSameCardIsNotSentTwice) {
  EXPECT_FALSE(ControlLaneCarriesCard(false, false, {100}, 100));
  EXPECT_FALSE(ControlLaneCarriesCard(false, false, {100, 100}, 100)) << "every device addressed was told";
}

TEST(ControlLaneCardTest, ANewerCardGoesOutOnce) {
  EXPECT_TRUE(ControlLaneCarriesCard(false, false, {100}, 200));
}

TEST(ControlLaneCardTest, AnOpenerOrAnAskCarriesItWhateverWasSent) {
  EXPECT_TRUE(ControlLaneCarriesCard(true, false, {100}, 100)) << "a session opener: the peer derives the session";
  EXPECT_TRUE(ControlLaneCarriesCard(false, true, {100}, 100)) << "the caller knows the peer may hold none of ours";
}

// #555: the account was told, but a device linked to it since was not — the record carries the card, once.
TEST(ControlLaneCardTest, ADeviceNotToldYetGetsItThoughItsSiblingsWere) {
  EXPECT_TRUE(ControlLaneCarriesCard(false, false, {100, std::nullopt}, 100));
  EXPECT_TRUE(ControlLaneCarriesCard(false, false, {200, 100}, 200)) << "one device has only an older one";
}

TEST(ControlLaneCardTest, ARecordAddressesItsDeviceCopiesOrEveryDeviceOfAnAccountCopy) {
  const std::vector<ControlLaneCopy> per_device{ControlLaneCopy{.recipient_device = "phone"},
                                                ControlLaneCopy{.recipient_device = "tablet"}};
  EXPECT_EQ(ControlLaneAddressedDevices(per_device, {"phone", "tablet", "laptop"}),
            (std::vector<std::string>{"phone", "tablet"}))
      << "a device left out of the record (no session) is not told by it";
  const std::vector<ControlLaneCopy> account_copy{ControlLaneCopy{}};
  EXPECT_EQ(ControlLaneAddressedDevices(account_copy, {"phone", "tablet"}),
            (std::vector<std::string>{"phone", "tablet"}))
      << "every device on the card reads an account copy";
  EXPECT_EQ(ControlLaneAddressedDevices(account_copy, {}), (std::vector<std::string>{""}))
      << "a first contact: the account, its devices unknown";
}

} // namespace
} // namespace pbr
