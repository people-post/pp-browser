#include "lib/net/ClientCompat.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>

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
    "crash_reports_url": "https://example.com/api/relay/v1/crash-reports",
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
  EXPECT_EQ(doc->crash_reports_url, "https://example.com/api/relay/v1/crash-reports");
}

TEST(ClientCompatParse, RejectsNonHttpsUpgradeUrl) {
  // upgrade_url is surfaced to the user as an "update now" link; a plain-http (or other scheme)
  // value must be dropped rather than shown/opened.
  const char* json = R"({
    "schema_version": 1,
    "upgrade_url": "http://example.com/upgrade"
  })";
  auto doc = ParseClientCompatDocument(json);
  ASSERT_TRUE(doc);
  EXPECT_TRUE(doc->upgrade_url.empty());
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
  EXPECT_EQ(ResolvedUpgradeUrl(doc, "https://example.com/releases", "linux"), "https://example.com/releases");
  doc.upgrade_url = "https://example.com/app";
  EXPECT_EQ(ResolvedUpgradeUrl(doc, "https://example.com/releases", "linux"), "https://example.com/app");
}

// `upgrade_urls`: this platform's link first, else `upgrade_url`, else the app's default.
TEST(ClientCompatUpgradeUrl, ThisPlatformsLinkFirst) {
  auto doc = ParseClientCompatDocument(R"({
    "schema_version": 1,
    "upgrade_url": "https://example.com/app",
    "upgrade_urls": {"ios": "https://apps.example.com/id1", "android": "https://play.example.com/x"}
  })");
  ASSERT_TRUE(doc);
  EXPECT_EQ(ResolvedUpgradeUrl(*doc, "https://example.com/releases", "ios"), "https://apps.example.com/id1");
  EXPECT_EQ(ResolvedUpgradeUrl(*doc, "https://example.com/releases", "android"), "https://play.example.com/x");
  EXPECT_EQ(ResolvedUpgradeUrl(*doc, "https://example.com/releases", "macos"), "https://example.com/app")
      << "no link of its own: the document's one";
  doc->upgrade_url.clear();
  EXPECT_EQ(ResolvedUpgradeUrl(*doc, "https://example.com/releases", "windows"), "https://example.com/releases");
}

// Only the known platforms' https links are kept (each is opened as an "update now" link); a malformed block is none.
TEST(ClientCompatParse, UpgradeUrlsKeepOnlyHttpsLinksOfKnownPlatforms) {
  auto doc = ParseClientCompatDocument(R"({
    "schema_version": 1,
    "upgrade_urls": {"ios": "http://apps.example.com/id1", "android": 5, "macos": "https://mac.example.com/a b",
                     "windows": "https://win.example.com/", "linux": "https://", "beos": "https://beos.example.com/"}
  })");
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->upgrade_urls, (std::map<std::string, std::string>{{"windows", "https://win.example.com/"}}));
  auto not_an_object = ParseClientCompatDocument(R"({"schema_version": 1, "upgrade_urls": "https://example.com/"})");
  ASSERT_TRUE(not_an_object);
  EXPECT_TRUE(not_an_object->upgrade_urls.empty());
}

TEST(ClientCompatCache, RoundTripsUpgradeUrls) {
  const auto dir = std::filesystem::temp_directory_path() / "pp-browser-client-compat-urls-test";
  std::filesystem::remove_all(dir);
  ClientCompatCacheEntry entry;
  entry.fetched_at_unix = 1'700'000'000;
  entry.document.schema_version = 1;
  entry.document.upgrade_urls = {{"ios", "https://apps.example.com/id1"}, {"linux", "https://example.com/linux"}};
  ASSERT_TRUE(SaveClientCompatCache(dir.string(), entry));
  auto loaded = LoadClientCompatCache(dir.string());
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->document.upgrade_urls, entry.document.upgrade_urls);
  std::filesystem::remove_all(dir);
}

// The org's relay refusing us (426 client_upgrade_required) is the authority: the update screen whatever the floor.
TEST(ClientCompatDecide, TheOrgRelayRefusingUsRequiresTheUpdate) {
  ClientCompatDocument doc;
  doc.schema_version = 1;
  EXPECT_EQ(DecideCompatUiAction("9.9.9", doc, /*relay_requires_upgrade=*/true), CompatUiAction::UpdateRequired);
  EXPECT_EQ(DecideCompatUiAction("9.9.9", doc, /*relay_requires_upgrade=*/false), CompatUiAction::None);
  doc.latest_client_version = "10.0.0";
  EXPECT_EQ(DecideCompatUiAction("9.9.9", doc, true), CompatUiAction::UpdateRequired);
}

/** A relay answering `doc` (or failing), counting its fetches. */
class FakeCompatClient : public IClientCompatClient {
public:
  Roe<ClientCompatDocument> Fetch() override {
    ++fetches;
    if (fail) {
      return Error("relay unreachable");
    }
    return doc;
  }
  ClientCompatDocument doc;
  bool fail = false;
  int fetches = 0;
};

class ClientCompatResolveTest : public ::testing::Test {
protected:
  void SetUp() override {
    // A directory of its own per test: ctest runs this fixture's tests at once, and on Windows one test's remove_all
    // fails on a file another still holds open (TEST_STRATEGY § Temp data dirs).
    dir_ = std::filesystem::temp_directory_path() / ("pp_client_compat_resolve_" + util::GenerateUuid());
    ClientCompatCacheEntry cached;
    cached.fetched_at_unix = kCachedAt;
    cached.document.schema_version = 1;
    cached.document.min_client_version = "0.1.0";
    ASSERT_TRUE(SaveClientCompatCache(dir_.string(), cached));
    relay_.doc.schema_version = 1;
    relay_.doc.min_client_version = "0.9.0";
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  static constexpr int64_t kCachedAt = 1'700'000'000;
  std::filesystem::path dir_;
  FakeCompatClient relay_;
};

TEST_F(ClientCompatResolveTest, AFreshCacheIsTakenAsItIsAtStartUp) {
  const auto result = ResolveClientCompat(&relay_, dir_.string(), kCachedAt + 60, CompatCheck::CacheFirst);
  ASSERT_TRUE(result.document);
  EXPECT_EQ(result.document->min_client_version, "0.1.0");
  EXPECT_EQ(result.fetched_at_unix, kCachedAt);
  EXPECT_FALSE(result.fetched);
  EXPECT_EQ(relay_.fetches, 0);
}

// A forced check (the app in front again, the org's relay answering 426) asks the relay even with a fresh cache.
TEST_F(ClientCompatResolveTest, AForcedCheckAsksTheRelayPastAFreshCache) {
  const int64_t now = kCachedAt + 60;
  const auto result = ResolveClientCompat(&relay_, dir_.string(), now, CompatCheck::Forced);
  ASSERT_TRUE(result.document);
  EXPECT_EQ(result.document->min_client_version, "0.9.0");
  EXPECT_EQ(result.fetched_at_unix, now);
  EXPECT_TRUE(result.fetched);
  EXPECT_EQ(relay_.fetches, 1);
  auto cached = LoadClientCompatCache(dir_.string());
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->document.min_client_version, "0.9.0") << "what it fetched is the cache now";
}

// Fail open as before: a forced check whose fetch fails applies the cached copy.
TEST_F(ClientCompatResolveTest, AForcedCheckThatFailsFallsBackToTheCache) {
  relay_.fail = true;
  const auto result = ResolveClientCompat(&relay_, dir_.string(), kCachedAt + 60, CompatCheck::Forced);
  ASSERT_TRUE(result.document);
  EXPECT_EQ(result.document->min_client_version, "0.1.0");
  EXPECT_EQ(result.fetched_at_unix, kCachedAt);
  EXPECT_FALSE(result.fetched);
  EXPECT_EQ(result.error, "relay unreachable");
  std::filesystem::remove_all(dir_);
  EXPECT_FALSE(ResolveClientCompat(&relay_, dir_.string(), kCachedAt, CompatCheck::Forced).document)
      << "nothing cached either: fail open with no document";
}

// The app in front again checks at most once per 15 minutes since the document it holds was fetched.
TEST(ClientCompatRecheck, AtMostOncePerFifteenMinutesSinceTheLastFetch) {
  const int64_t fetched = 1'700'000'000;
  EXPECT_FALSE(ClientCompatRecheckDue(fetched, fetched));
  EXPECT_FALSE(ClientCompatRecheckDue(fetched, fetched + kClientCompatRecheckSeconds - 1));
  EXPECT_TRUE(ClientCompatRecheckDue(fetched, fetched + kClientCompatRecheckSeconds));
  EXPECT_TRUE(ClientCompatRecheckDue(0, fetched)) << "nothing fetched yet";
  EXPECT_TRUE(ClientCompatRecheckDue(fetched, fetched - 1)) << "a clock stepped back is not trusted";
  EXPECT_EQ(kClientCompatRecheckSeconds, 15 * 60);
}

} // namespace
