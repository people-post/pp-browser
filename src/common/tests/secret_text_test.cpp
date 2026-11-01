#include "common/Secret.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace pbr {
namespace {

// A secret never becomes text by accident: no streaming (a logger streams what it is given), no conversion to a
// string.
template <class T>
concept Streams = requires(std::ostream& out, const T& value) { out << value; };
static_assert(!Streams<SecretText>);
static_assert(!std::is_convertible_v<SecretText, std::string>);
static_assert(!std::is_convertible_v<SecretText, std::string_view>);
static_assert(!std::is_convertible_v<SecretText, const char*>);
// The other way is free: text becomes a secret where it enters.
static_assert(std::is_convertible_v<std::string, SecretText>);

TEST(SecretTextTest, RevealIsTheOnlyWayToTheText) {
  const SecretText pin("4321");
  EXPECT_EQ(pin.Reveal(), "4321");
  EXPECT_EQ(pin.size(), 4u);
  EXPECT_FALSE(pin.empty());
}

TEST(SecretTextTest, ComparesByContent) {
  EXPECT_TRUE(SecretText("4321") == SecretText("4321"));
  EXPECT_FALSE(SecretText("4321") == SecretText("4322"));
  EXPECT_FALSE(SecretText("4321") == SecretText("43210"));
}

TEST(SecretTextTest, AMovedOrClearedSecretHoldsNothing) {
  SecretText from("the whole account");
  SecretText to(std::move(from));
  EXPECT_EQ(to.Reveal(), "the whole account");
  EXPECT_TRUE(from.empty());  // NOLINT(bugprone-use-after-move): wiped on purpose
  to.clear();
  EXPECT_TRUE(to.empty());
  SecretText replaced("old");
  replaced = SecretText("new");
  EXPECT_EQ(replaced.Reveal(), "new");
}

} // namespace
} // namespace pbr
