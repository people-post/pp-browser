#include "base/data/SchemaVersion.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

TEST(SchemaVersionTest, AcceptsCurrentSchemaVersion) {
  const nlohmann::json good = {{"schema_version", pbr::SchemaVersion::kCurrentSchemaVersion}};
  const auto result =
      pbr::SchemaVersion::Validate(good, pbr::SchemaVersion::kCurrentSchemaVersion, "good.json");
  EXPECT_TRUE(static_cast<bool>(result));
}

TEST(SchemaVersionTest, RejectsUnsupportedSchemaVersion) {
  const nlohmann::json bad = {{"schema_version", 99}};
  const auto result = pbr::SchemaVersion::Validate(bad, 1, "test.json");
  EXPECT_TRUE(result.isError());
}

TEST(SchemaVersionTest, RejectsMissingSchemaVersion) {
  const nlohmann::json missing = nlohmann::json::object();
  const auto result = pbr::SchemaVersion::Validate(missing, 1, "missing.json");
  EXPECT_TRUE(result.isError());
}
