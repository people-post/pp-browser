#include "lib/net/ClientCompat.h"

#include "common/Utilities.h"
#include "foundation/error/AppError.h"

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

// T15: a newer schema_version is read for the fields this reader knows (the relay only adds fields); none or below 1
// is no document.
TEST(ClientCompatParse, ANewerSchemaVersionIsReadForTheFieldsItKnows) {
  auto doc = ParseClientCompatDocument(R"({"schema_version": 99, "min_client_version": "2.0.0", "novel": {"x": 1}})");
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->schema_version, 99);
  EXPECT_EQ(doc->min_client_version, "2.0.0");
  EXPECT_FALSE(ParseClientCompatDocument(R"({"schema_version": 0})"));
  EXPECT_FALSE(ParseClientCompatDocument(R"({"min_client_version": "2.0.0"})"));
}

// A4 / A1 / A2 / O2: the release-1 fields, each read alone (a malformed one is left out, the document stands).
TEST(ClientCompatParse, TheReleaseOneFieldsAreRead) {
  auto doc = ParseClientCompatDocument(R"({
    "schema_version": 1,
    "relay_version": "1.0.0",
    "app_range": {"min": "1.0.0", "max": null},
    "node_range": {"min": "v10.0.0", "max": "10.2.0"},
    "min_node_version": "10.0.0",
    "switches": {"calls.new_path": true, "feed": false, "Bad Name": true, "-x": true, "not_bool": "yes", "n": 1}
  })");
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->relay_version, "1.0.0");
  ASSERT_TRUE(doc->app_range);
  EXPECT_EQ(doc->app_range->min, "1.0.0");
  EXPECT_EQ(doc->app_range->max, "");
  ASSERT_TRUE(doc->node_range);
  EXPECT_EQ(doc->node_range->min, "") << "a bound that is not a release version is no bound, as the relay sends";
  EXPECT_EQ(doc->node_range->max, "10.2.0");
  EXPECT_EQ(doc->min_node_version, "10.0.0");
  EXPECT_EQ(doc->switches, (std::map<std::string, bool>{{"calls.new_path", true}, {"feed", false}}));

  auto bare = ParseClientCompatDocument(R"({"schema_version": 1, "switches": [true], "app_range": "1.0.0"})");
  ASSERT_TRUE(bare) << "a malformed field leaves the document standing";
  EXPECT_TRUE(bare->switches.empty());
  EXPECT_FALSE(bare->app_range);
  EXPECT_TRUE(bare->relay_version.empty());
}

TEST(ClientCompatParse, AtMostSixtyFourSwitchesAreKept) {
  std::string json = R"({"schema_version": 1, "switches": {)";
  for (int i = 0; i < 80; ++i) {
    json += (i ? "," : "") + std::string("\"s") + std::to_string(i) + "\":true";
  }
  json += "}}";
  auto doc = ParseClientCompatDocument(json);
  ASSERT_TRUE(doc);
  EXPECT_EQ(doc->switches.size(), kMaxRemoteSwitches);
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

// T15: a floor compares the core (a release candidate meets the floor of its release, as the relay compares); "a newer
// version exists" compares by release precedence (a release candidate hears that its release is out).
TEST(ClientCompatDecide, APrereleaseMeetsItsReleasesFloorAndHearsItIsOut) {
  ClientCompatDocument doc;
  doc.schema_version = 1;
  doc.min_client_version = "1.0.0";
  doc.latest_client_version = "1.0.0";
  EXPECT_EQ(DecideCompatUiAction("1.0.0-rc16", doc), CompatUiAction::SoftUpdateAvailable);
  EXPECT_EQ(DecideCompatUiAction("1.0.0", doc), CompatUiAction::None);
  EXPECT_EQ(DecideCompatUiAction("0.9.9", doc), CompatUiAction::UpdateRequired);
  doc.latest_client_version = "1.0.0-rc.3";
  EXPECT_EQ(DecideCompatUiAction("1.0.0-rc.2", doc), CompatUiAction::SoftUpdateAvailable);
  EXPECT_EQ(DecideCompatUiAction("1.0.0-rc.3", doc), CompatUiAction::None);
}

// T15: a floor that is not a version is no floor (fail open: a typo in the relay's config must not lock every app
// out), and it is told apart for the log.
TEST(ClientCompatDecide, AnUnreadableFloorIsNoFloor) {
  ClientCompatDocument doc;
  doc.schema_version = 1;
  doc.min_client_version = "one.zero";
  EXPECT_EQ(DecideCompatUiAction("0.0.1", doc), CompatUiAction::None);
  EXPECT_TRUE(ClientCompatFloorUnreadable(doc));
  doc.min_client_version = "";
  EXPECT_FALSE(ClientCompatFloorUnreadable(doc));
  doc.min_client_version = "v1.0.0";
  EXPECT_FALSE(ClientCompatFloorUnreadable(doc));
}

TEST(ClientCompatSemver, ReleasePrecedence) {
  EXPECT_LT(CompareReleaseVersions("1.0.0-rc.2", "1.0.0"), 0);
  EXPECT_GT(CompareReleaseVersions("1.0.0", "1.0.0-rc16"), 0);
  EXPECT_LT(CompareReleaseVersions("1.0.0-rc2", "1.0.0-rc16"), 0) << "letters then a number, as our tags are written";
  EXPECT_LT(CompareReleaseVersions("1.0.0-rc.2", "1.0.0-rc.10"), 0) << "numbers numerically";
  EXPECT_LT(CompareReleaseVersions("1.0.0-alpha", "1.0.0-beta"), 0);
  EXPECT_LT(CompareReleaseVersions("1.0.0-1", "1.0.0-alpha"), 0) << "numbers below words";
  EXPECT_LT(CompareReleaseVersions("1.0.0-rc", "1.0.0-rc.1"), 0) << "fewer identifiers first";
  EXPECT_EQ(CompareReleaseVersions("v1.0.0+abc", "1.0.0"), 0) << "build metadata is ignored";
  EXPECT_EQ(CompareReleaseVersions("1.0.0-rc.1+abc", "1.0.0-rc.1"), 0);
  EXPECT_LT(CompareReleaseVersions("0.9.9", "1.0.0-rc1"), 0) << "the core first";
  EXPECT_LT(CompareReleaseVersions("garbage", "1.0.0"), 0) << "invalid below valid, as the core compares";
}

TEST(ClientCompatNodeFloor, ANodeBelowTheFloorByItsCore) {
  ClientCompatDocument doc;
  EXPECT_FALSE(NodeBelowFloor("1.0.0", doc)) << "no floor";
  doc.min_node_version = "10.0.0";
  EXPECT_TRUE(NodeBelowFloor("9.9.9", doc));
  EXPECT_TRUE(NodeBelowFloor("1.0.0-rc16", doc));
  EXPECT_FALSE(NodeBelowFloor("10.0.0-rc1", doc)) << "a prerelease meets its release's floor, as the relay compares";
  EXPECT_FALSE(NodeBelowFloor("10.0.1", doc));
  EXPECT_FALSE(NodeBelowFloor("unparsable", doc)) << "not judged, as the relay does not";
  doc.min_node_version = "ten";
  EXPECT_FALSE(NodeBelowFloor("1.0.0", doc)) << "an unreadable floor is none";
}

// O1: the header's value as the relay reads it (SPECS § X-PP-Client).
TEST(ClientCompatHeader, TheVersionHeaderValue) {
  EXPECT_EQ(RelayClientHeaderValue("pp-app", "1.0.0-rc16", "linux"), "pp-app/1.0.0-rc16 linux");
  EXPECT_EQ(RelayClientHeaderValue("PP-Node", "v10.0.0+build.7", "Linux"), "pp-node/10.0.0 linux")
      << "the release version core: no v, no build metadata; lowercase";
  EXPECT_EQ(ClientHeaderVersion("1.0.0-rc.2"), "1.0.0-rc.2");
  EXPECT_EQ(ClientHeaderVersion("1.0.0-averyveryverylongprerelease"), "1.0.0") << "over 16 characters: dropped";
  EXPECT_EQ(ClientHeaderVersion("1.0.0-rc_1"), "1.0.0") << "characters the relay does not read: dropped";
  EXPECT_EQ(ClientHeaderVersion("nope"), "");
  EXPECT_EQ(RelayClientHeaderValue("pp-app", "nope", "ios"), "") << "nothing is sent rather than an invalid value";
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

// A4: the cache keeps every release-1 field and names its own file version; a newer app's file is no cache here and is
// never written over, a file from before the version is read as it is.
TEST(ClientCompatCache, KeepsTheReleaseOneFieldsAndItsFileVersion) {
  const auto dir = std::filesystem::temp_directory_path() / ("pp-browser-client-compat-r1-" + util::GenerateUuid());
  std::filesystem::create_directories(dir);
  ClientCompatCacheEntry entry;
  entry.fetched_at_unix = 1'700'000'000;
  entry.document.schema_version = 2;
  entry.document.relay_version = "1.2.3";
  entry.document.app_range = ClientCompatRange{.min = "1.0.0", .max = ""};
  entry.document.node_range = ClientCompatRange{.min = "10.0.0", .max = "10.9.0"};
  entry.document.min_node_version = "10.0.0";
  entry.document.switches = {{"a", true}, {"b", false}};
  ASSERT_TRUE(SaveClientCompatCache(dir.string(), entry));

  std::ifstream in(dir / "client_compat.json");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(text.find("\"schema_version\": 1,"), std::string::npos) << "the file's own version: " << text;

  auto loaded = LoadClientCompatCache(dir.string());
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->document.schema_version, 2);
  EXPECT_EQ(loaded->document.relay_version, "1.2.3");
  ASSERT_TRUE(loaded->document.app_range);
  EXPECT_EQ(loaded->document.app_range->min, "1.0.0");
  EXPECT_EQ(loaded->document.app_range->max, "");
  ASSERT_TRUE(loaded->document.node_range);
  EXPECT_EQ(loaded->document.node_range->max, "10.9.0");
  EXPECT_EQ(loaded->document.min_node_version, "10.0.0");
  EXPECT_EQ(loaded->document.switches, entry.document.switches);

  {
    std::ofstream newer(dir / "client_compat.json", std::ios::trunc);
    newer << R"({"schema_version": 2, "fetched_at_unix": 1, "document": {"schema_version": 1}})";
  }
  auto refused = LoadClientCompatCache(dir.string());
  ASSERT_FALSE(refused);
  EXPECT_TRUE(AppError::IsNewerFormat(refused.error()));
  EXPECT_FALSE(SaveClientCompatCache(dir.string(), entry)) << "a newer app's file is never written over";
  auto still = LoadClientCompatCache(dir.string());
  ASSERT_FALSE(still);
  EXPECT_TRUE(AppError::IsNewerFormat(still.error()));
  {
    std::ofstream older(dir / "client_compat.json", std::ios::trunc);
    older << R"({"fetched_at_unix": 1, "document": {"schema_version": 1, "min_client_version": "0.1.0"}})";
  }
  auto before = LoadClientCompatCache(dir.string());
  ASSERT_TRUE(before) << "a file from before A4 is read";
  EXPECT_EQ(before->document.min_client_version, "0.1.0");
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

// Each of the five platforms gets its own link, whichever platform runs the test.
TEST(ClientCompatUpgradeUrl, EveryPlatformGetsItsOwnLink) {
  auto doc = ParseClientCompatDocument(R"({
    "schema_version": 1,
    "upgrade_url": "https://example.com/app",
    "upgrade_urls": {"ios": "https://example.com/ios", "android": "https://example.com/android",
                     "macos": "https://example.com/macos", "windows": "https://example.com/windows",
                     "linux": "https://example.com/linux"}
  })");
  ASSERT_TRUE(doc);
  for (const char* platform : {"ios", "android", "macos", "windows", "linux"}) {
    EXPECT_EQ(ResolvedUpgradeUrl(*doc, "https://example.com/releases", platform),
              std::string("https://example.com/") + platform)
        << platform;
  }
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
// T14 / RC018 part 2 default: the org's relay refusing us needs the update whatever the document says, as the held-up
// card; the full screen stays the document's floor's.
TEST(ClientCompatDecide, TheOrgRelayRefusingUsNeedsTheUpdate) {
  ClientCompatDocument doc;
  doc.schema_version = 1;
  EXPECT_EQ(DecideCompatUiAction("9.9.9", doc, /*relay_requires_upgrade=*/true), CompatUiAction::UpdateNeeded);
  EXPECT_EQ(DecideCompatUiAction("9.9.9", doc, /*relay_requires_upgrade=*/false), CompatUiAction::None);
  doc.latest_client_version = "10.0.0";
  EXPECT_EQ(DecideCompatUiAction("9.9.9", doc, true), CompatUiAction::UpdateNeeded);
  doc.min_client_version = "10.0.0";
  EXPECT_EQ(DecideCompatUiAction("9.9.9", doc, true), CompatUiAction::UpdateRequired) << "below the floor: full screen";
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
