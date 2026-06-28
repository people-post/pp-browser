#include "base/ai/RmlValidator.h"

#include <gtest/gtest.h>

TEST(RmlValidatorTest, AcceptsValidRml) {
  const std::string valid = "<rml><body><button data-event-click=\"go()\"/></body></rml>";
  const auto ok = pbr::RmlValidator::ValidateRml(valid);
  EXPECT_TRUE(ok.ok);
}

TEST(RmlValidatorTest, RejectsDisallowedScriptTag) {
  const std::string bad = "<rml><script>alert(1)</script></rml>";
  const auto bad_result = pbr::RmlValidator::ValidateRml(bad);
  EXPECT_FALSE(bad_result.ok);
}
