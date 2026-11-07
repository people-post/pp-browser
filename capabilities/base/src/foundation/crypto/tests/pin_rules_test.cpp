// The PIN's kind (DEV/PLAN-pin-gate-redesign.md B): 4–8 digits are a digit PIN (the keypad), anything else of at least
// four characters a text PIN, shorter not a PIN.
#include "foundation/crypto/PinRules.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(PinRulesTest, DigitsOfEachKind) {
  EXPECT_EQ(PinDigitsOf(""), std::nullopt);
  EXPECT_EQ(PinDigitsOf("123"), std::nullopt) << "three digits are too short";
  EXPECT_EQ(PinDigitsOf("1234"), 4);
  EXPECT_EQ(PinDigitsOf("12345678"), 8);
  EXPECT_EQ(PinDigitsOf("123456789"), 0) << "nine digits are typed in the field";
  EXPECT_EQ(PinDigitsOf("12a4"), 0) << "a letter makes a text PIN";
  EXPECT_EQ(PinDigitsOf("abc"), std::nullopt);
}

TEST(PinRulesTest, OnlyFourToEightIsAKeypadLength) {
  EXPECT_FALSE(IsDigitPinLength(std::nullopt));
  EXPECT_FALSE(IsDigitPinLength(0));
  EXPECT_FALSE(IsDigitPinLength(3));
  EXPECT_TRUE(IsDigitPinLength(4));
  EXPECT_TRUE(IsDigitPinLength(8));
  EXPECT_FALSE(IsDigitPinLength(9));
}

} // namespace
} // namespace pbr
