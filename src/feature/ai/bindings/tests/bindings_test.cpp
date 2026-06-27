#include "feature/ai/bindings/BindingsManifest.h"

#include <gtest/gtest.h>

TEST(BindingsManifestTest, ParsesActionManifest) {
  const char* json = R"({
    "actions": {
      "search_users": {
        "tool": "user_search",
        "params": {"query": "{{input:#query}}"},
        "result_bind": "results",
        "risk": "read"
      }
    }
  })";

  pbr::BindingsManifest manifest;
  const auto parse_result = pbr::BindingsManifest::Parse(json, manifest);
  ASSERT_TRUE(parse_result);

  const auto* action = manifest.Find("search_users");
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->tool, "user_search");
  EXPECT_EQ(action->result_bind, "results");

  const auto validate_result = pbr::BindingsManifest::Validate(json);
  EXPECT_TRUE(validate_result.ok);
}
