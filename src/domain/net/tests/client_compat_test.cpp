#include "domain/net/ClientCompat.h"
#include "foundation/runtime/AppVersion.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

using namespace pbr;

TEST(ClientCompatSemver, ParsesCoreAndStripsPrerelease) {
  const SemverCore a = ParseSemverCore("1.2.3");
  ASSERT_TRUE(a.ok);
  EXPECT_EQ(a.major, 1);
  EXPECT_EQ(a.minor, 2);
  EXPECT_EQ(a.patch, 3);

  const SemverCore b = ParseSemverCore("v0.4.2-rc1");
  ASSERT_TRUE(b.ok);
  EXPECT_EQ(b.major, 0);
  EXPECT_EQ(b.minor, 4);
  EXPECT_EQ(b.patch, 2);

  EXPECT_LT(CompareSemverCore("0.3.0", "0.3.1"), 0);
  EXPECT_EQ(CompareSemverCore("1.0.0-rc1", "1.0.0"), 0);
  EXPECT_GT(CompareSemverCore("2.0.0", "1.9.9"), 0);
}

TEST(ClientCompatParse, HappyPathIgnoresUnknownKeys) {
  const char* json = R"({
    "schema_version": 1,
    "min_client_version": "0.3.0",
    "latest_client_version": "0.4.2",
    "min_protocol_gen": 2,
    "upgrade_url": "https://example.com/upgrade",
    "message": "Please update",
    "extra_future_field": true
  })";
  auto doc = ParseClientCompatDocument(json);
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->schema_version, 1);
  EXPECT_EQ(doc->min_client_version, "0.3.0");
  EXPECT_EQ(doc->latest_client_version, "0.4.2");
  EXPECT_EQ(doc->min_protocol_gen, 2);
  EXPECT_EQ(doc->upgrade_url, "https://example.com/upgrade");
  EXPECT_EQ(doc->message, "Please update");
}

TEST(ClientCompatParse, RejectsNewerSchemaVersion) {
  auto doc = ParseClientCompatDocument(R"({"schema_version": 99})");
  ASSERT_FALSE(doc);
}

TEST(ClientCompatParse, SupportEnabledHappyPath) {
  const char* json = R"({
    "schema_version": 1,
    "support": {
      "enabled": true,
      "account_id": "account:support123",
      "display_name": "PP Support"
    }
  })";
  auto doc = ParseClientCompatDocument(json);
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->support.has_value());
  EXPECT_TRUE(doc->support->enabled);
  EXPECT_EQ(doc->support->account_id, "account:support123");
  EXPECT_EQ(doc->support->display_name, "PP Support");
}

TEST(ClientCompatParse, SupportDisabledIsPresentButOff) {
  const char* json = R"({
    "schema_version": 1,
    "support": {
      "enabled": false,
      "account_id": "account:support123",
      "display_name": "PP Support"
    }
  })";
  auto doc = ParseClientCompatDocument(json);
  ASSERT_TRUE(doc);
  ASSERT_TRUE(doc->support.has_value());
  EXPECT_FALSE(doc->support->enabled);
  EXPECT_TRUE(doc->support->account_id.empty());
}

TEST(ClientCompatParse, SupportMissingOmitsBlock) {
  auto doc = ParseClientCompatDocument(R"({"schema_version": 1})");
  ASSERT_TRUE(doc);
  EXPECT_FALSE(doc->support.has_value());
}

TEST(ClientCompatParse, SupportEnabledWithoutAccountIdOmits) {
  const char* json = R"({
    "schema_version": 1,
    "support": {
      "enabled": true,
      "account_id": "",
      "display_name": "PP Support"
    }
  })";
  auto doc = ParseClientCompatDocument(json);
  ASSERT_TRUE(doc);
  EXPECT_FALSE(doc->support.has_value());
}

TEST(ClientCompatParse, SupportMalformedDoesNotFailDocument) {
  auto doc = ParseClientCompatDocument(R"({"schema_version": 1, "support": "nope"})");
  ASSERT_TRUE(doc);
  EXPECT_FALSE(doc->support.has_value());
}

TEST(ClientCompatCache, RoundTripsSupportBlock) {
  const auto dir = std::filesystem::temp_directory_path() / "pp-browser-client-compat-support-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  ClientCompatCacheEntry entry;
  entry.fetched_at_unix = 1'700'000'000;
  entry.document.schema_version = 1;
  ClientCompatSupport support;
  support.enabled = true;
  support.account_id = "account:support";
  support.display_name = "PP Support";
  entry.document.support = support;
  ASSERT_TRUE(SaveClientCompatCache(dir.string(), entry));

  auto loaded = LoadClientCompatCache(dir.string());
  ASSERT_TRUE(loaded);
  ASSERT_TRUE(loaded->document.support.has_value());
  EXPECT_TRUE(loaded->document.support->enabled);
  EXPECT_EQ(loaded->document.support->account_id, "account:support");
  EXPECT_EQ(loaded->document.support->display_name, "PP Support");

  std::filesystem::remove_all(dir);
}

TEST(ClientCompatDecide, UpdateRequiredSoftAndNone) {
  ClientCompatDocument doc;
  doc.schema_version = 1;
  doc.min_client_version = "0.3.0";
  doc.latest_client_version = "0.4.0";

  EXPECT_EQ(DecideCompatUiAction("0.2.9", doc), CompatUiAction::UpdateRequired);
  EXPECT_EQ(DecideCompatUiAction("0.3.0", doc), CompatUiAction::SoftUpdateAvailable);
  EXPECT_EQ(DecideCompatUiAction("0.3.5-rc1", doc), CompatUiAction::SoftUpdateAvailable);
  EXPECT_EQ(DecideCompatUiAction("0.4.0", doc), CompatUiAction::None);
  EXPECT_EQ(DecideCompatUiAction("1.0.0", doc), CompatUiAction::None);
}

TEST(ClientCompatDecide, EmptyFloorsMeanNone) {
  ClientCompatDocument doc;
  doc.schema_version = 1;
  EXPECT_EQ(DecideCompatUiAction("0.1.0", doc), CompatUiAction::None);
}

TEST(ClientCompatCache, RoundTripAndFreshness) {
  const auto dir = std::filesystem::temp_directory_path() / "pp-browser-client-compat-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  ClientCompatCacheEntry entry;
  entry.fetched_at_unix = 1'700'000'000;
  entry.document.schema_version = 1;
  entry.document.min_client_version = "0.2.0";
  entry.document.latest_client_version = "0.2.1";
  ASSERT_TRUE(SaveClientCompatCache(dir.string(), entry));

  auto loaded = LoadClientCompatCache(dir.string());
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->fetched_at_unix, entry.fetched_at_unix);
  EXPECT_EQ(loaded->document.min_client_version, "0.2.0");
  EXPECT_TRUE(ClientCompatCacheFresh(*loaded, entry.fetched_at_unix + 60));
  EXPECT_FALSE(ClientCompatCacheFresh(*loaded, entry.fetched_at_unix + kClientCompatCacheTtlSeconds + 1));

  std::filesystem::remove_all(dir);
}

TEST(ClientCompatUpgradeUrl, FallsBackToDefault) {
  ClientCompatDocument doc;
  EXPECT_EQ(ResolvedUpgradeUrl(doc), kDefaultUpgradeUrl);
  doc.upgrade_url = "https://example.com/app";
  EXPECT_EQ(ResolvedUpgradeUrl(doc), "https://example.com/app");
}

TEST(AppVersion, ConstantsPresent) {
  EXPECT_FALSE(std::string(AppVersionString()).empty());
  EXPECT_GE(kProtocolGen, 1);
  EXPECT_GE(kMinPeerProtocolGen, 1);
}

} // namespace
