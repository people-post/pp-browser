#include "base/net/ClientCompat.h"
#include "base/net/PeerProtocolCompat.h"
#include "base/people/ContactTypes.h"
#include "base/platform/AppVersion.h"

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

TEST(PeerProtocolCompat, LocalTooOldAndPeerTooOld) {
  DirectoryHit peer;
  peer.protocol_gen = 2;
  peer.min_peer_protocol_gen = 2;
  EXPECT_EQ(EvaluatePeerProtocolCompat(peer, /*local=*/1, /*min=*/1), PeerProtocolCompat::LocalTooOld);

  peer.protocol_gen = 1;
  peer.min_peer_protocol_gen = 1;
  EXPECT_EQ(EvaluatePeerProtocolCompat(peer, /*local=*/2, /*min=*/2), PeerProtocolCompat::PeerTooOld);

  peer.protocol_gen = std::nullopt;
  peer.min_peer_protocol_gen = std::nullopt;
  EXPECT_EQ(EvaluatePeerProtocolCompat(peer, 1, 1), PeerProtocolCompat::Compatible);
}

TEST(AppVersion, ConstantsPresent) {
  EXPECT_FALSE(std::string(AppVersionString()).empty());
  EXPECT_GE(kProtocolGen, 1);
  EXPECT_GE(kMinPeerProtocolGen, 1);
}

} // namespace
