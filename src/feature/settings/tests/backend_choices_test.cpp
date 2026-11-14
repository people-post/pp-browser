// Which services the "Service" row offers: QA needs both a QA address in the build and the tester options.
#include "feature/settings/BackendChoices.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

std::vector<std::string> Offered(const bool qa_compiled, const bool tester) {
  std::vector<std::string> ids;
  for (const char* id : BackendChoicesOffered(qa_compiled, tester)) {
    ids.push_back(id);
  }
  return ids;
}

} // namespace

TEST(BackendChoicesTest, QaNeedsTheBuildAndTheTesterOptions) {
  EXPECT_EQ(Offered(true, true), (std::vector<std::string>{"prod", "qa", "custom"}));
  EXPECT_EQ(Offered(true, false), (std::vector<std::string>{"prod", "custom"}));
  EXPECT_EQ(Offered(false, true), (std::vector<std::string>{"prod", "custom"}));
  EXPECT_EQ(Offered(false, false), (std::vector<std::string>{"prod", "custom"}));
}

TEST(BackendChoicesTest, ProductionAndCustomAreAlwaysThere) {
  for (const bool qa : {false, true}) {
    for (const bool tester : {false, true}) {
      const std::vector<std::string> ids = Offered(qa, tester);
      EXPECT_EQ(ids.front(), "prod");
      EXPECT_EQ(ids.back(), "custom");
    }
  }
}

} // namespace pbr
