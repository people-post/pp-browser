#include "gui/contacts/AddFromSearchDecision.h"

#include <gtest/gtest.h>

namespace pbr {

TEST(AddFromSearchDecisionTest, KnownContactWinsOverEverything) {
  EXPECT_EQ(DecideAddFromSearch(true, true), AddFromSearchDecision::AlreadyContact);
  EXPECT_EQ(DecideAddFromSearch(false, true), AddFromSearchDecision::AlreadyContact);
}

TEST(AddFromSearchDecisionTest, NameSearchAsksFirst) {
  EXPECT_EQ(DecideAddFromSearch(false, false), AddFromSearchDecision::ConfirmByName);
}

TEST(AddFromSearchDecisionTest, ExactIdAddsDirectly) {
  EXPECT_EQ(DecideAddFromSearch(true, false), AddFromSearchDecision::AddDirect);
}

} // namespace pbr
