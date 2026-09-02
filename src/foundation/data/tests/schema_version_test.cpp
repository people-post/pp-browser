#include "foundation/data/SchemaVersion.h"

#include <gtest/gtest.h>

TEST(SchemaVersionTest, AcceptsCurrentSchemaVersion) {
  pbr::Object good;
  good.set("schema_version", static_cast<int64_t>(pbr::SchemaVersion::kCurrentSchemaVersion));
  const auto result =
      pbr::SchemaVersion::Validate(good, pbr::SchemaVersion::kCurrentSchemaVersion, "good.json");
  EXPECT_TRUE(static_cast<bool>(result));
}

TEST(SchemaVersionTest, RejectsUnsupportedSchemaVersion) {
  pbr::Object bad;
  bad.set("schema_version", static_cast<int64_t>(99));
  const auto result = pbr::SchemaVersion::Validate(bad, 1, "test.json");
  EXPECT_TRUE(result.isError());
}

TEST(SchemaVersionTest, RejectsMissingSchemaVersion) {
  const pbr::Object missing;
  const auto result = pbr::SchemaVersion::Validate(missing, 1, "missing.json");
  EXPECT_TRUE(result.isError());
}
