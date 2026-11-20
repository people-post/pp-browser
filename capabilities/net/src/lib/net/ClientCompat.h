#pragma once

#include "common/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

struct SemverCore {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool ok = false;
};

/** Parse MAJOR.MINOR.PATCH; ignore optional leading `v` and any `-` / `+` suffix. */
SemverCore ParseSemverCore(std::string_view version);

/** -1 if a < b, 0 if equal, 1 if a > b. Invalid versions compare as less than valid. */
int CompareSemverCore(const SemverCore& a, const SemverCore& b);
int CompareSemverCore(std::string_view a, std::string_view b);

/**
 * Release precedence (release-compatibility T15; SERVICE_ENDPOINTS § Client compatibility): the core as
 * CompareSemverCore, then a prerelease is below its release (`1.0.0-rc.2` < `1.0.0`) and prereleases compare by their
 * dot-separated identifiers as semver 2.0 says (numbers numerically, below words; fewer identifiers first), with one
 * addition for how our tags are written: an identifier of letters then digits (`rc16`) compares by its letters, then
 * by its number (`rc2` < `rc16`). Build metadata (`+…`) is ignored. Invalid versions compare as CompareSemverCore.
 * Used for "a newer version exists" only; a floor (min_client_version, min_node_version) compares the core, as the
 * relay does, so a release candidate meets the floor of the version it is a candidate for.
 */
int CompareReleaseVersions(std::string_view a, std::string_view b);

/**
 * The version the relay's `X-PP-Client` header carries (SPECS: `X.Y.Z` with an optional `-prerelease` of up to 16
 * characters of `[0-9A-Za-z.]`): `version`'s core and prerelease, its build metadata and leading `v` dropped; a
 * prerelease the relay would not read is dropped too. Empty when `version` has no readable core.
 */
std::string ClientHeaderVersion(std::string_view version);

/**
 * `X-PP-Client`'s value (O1; SPECS § X-PP-Client): `<product>/<ClientHeaderVersion(version)> <os>`, product and os
 * lowercased (`pp-app/1.0.0-rc16 linux`, `pp-node/1.0.0 linux`). Empty when the version has no readable core.
 */
std::string RelayClientHeaderValue(std::string_view product, std::string_view version, std::string_view os);

/**
 * The highest `schema_version` this reader was written for. A document naming a higher one is still read for the
 * fields this reader knows (T15: the relay adds fields without breaking release 1); unknown fields are ignored.
 */
inline constexpr int kClientCompatSchemaVersion = 1;
inline constexpr int64_t kClientCompatCacheTtlSeconds = 6 * 60 * 60;
/** The app in front again asks anew once the document it holds was fetched this long ago (a forced check). */
inline constexpr int64_t kClientCompatRecheckSeconds = 15 * 60;
/** While the app runs, a forced check once the document held is this old (T15: a running app sees a new floor). */
inline constexpr int64_t kClientCompatTimerRecheckSeconds = 6 * 60 * 60;
/**
 * The `client_compat.json` file's own format version (A4; DATA_LAYOUT.md). A newer app's file reads as no cache and is
 * never written over; a file without one (before A4) is read as it is.
 */
inline constexpr int kClientCompatCacheFileVersion = 1;
/** At most this many remote switches are kept (the relay answers at most 64). */
inline constexpr size_t kMaxRemoteSwitches = 64;

/**
 * A range of versions a relay supports (`app_range`, `node_range`; RC018 part 1 item 3): each bound a release version
 * `X.Y.Z`, empty for no bound (the relay's `null`).
 */
struct ClientCompatRange {
  std::string min;
  std::string max;
};

/** Nested app Support discovery on client-compat (product help desk; omit / disabled = no entry). */
struct ClientCompatSupport {
  bool enabled = false;
  std::string account_id;
  std::string display_name;
};

struct ClientCompatDocument {
  int schema_version = 0;
  std::string min_client_version;
  std::string latest_client_version;
  int min_protocol_gen = 1;
  std::string upgrade_url;
  /**
   * `upgrade_urls`: the update link per platform (`ios`, `android`, `macos`, `windows`, `linux`), each an https URL;
   * preferred over `upgrade_url` on its platform (ResolvedUpgradeUrl). Other keys and values are dropped.
   */
  std::map<std::string, std::string> upgrade_urls;
  std::string message;
  /**
   * Optional absolute ingest URL for opt-in crash reports.
   * Empty → client uses `{relay.base_url}/v1/crash-reports`.
   */
  std::string crash_reports_url;
  std::optional<ClientCompatSupport> support;
  /** The relay's own build version (`relay_version`: `X.Y.Z`, `dev.<date>.<n>` or `0.0.0-local`); empty: not sent. */
  std::string relay_version;
  /** The app versions the relay supports (`app_range`); none: not sent. */
  std::optional<ClientCompatRange> app_range;
  /** The pp-node versions the relay supports (`node_range`); none: not sent. */
  std::optional<ClientCompatRange> node_range;
  /** The oldest pp-node the relay serves (`min_node_version`); empty: no floor. Read by pp-node (A2). */
  std::string min_node_version;
  /**
   * Remote switches (`switches`, O2): name to on / off, as the relay sent them (names of 1 to 64 of `a-z0-9_.-`,
   * starting with a letter or digit; a value that is not a boolean, or a name not of that form, is dropped; at most
   * kMaxRemoteSwitches). Read through RemoteSwitches, never directly by a feature.
   */
  std::map<std::string, bool> switches;
};

enum class CompatUiAction {
  None = 0,
  SoftUpdateAvailable,
  UpdateRequired,
};

Roe<ClientCompatDocument> ParseClientCompatDocument(std::string_view json_text);

/**
 * The update link to show: the document's for `platform` (Platform::OsName()), else its `upgrade_url`, else
 * `fallback` (the app's releases page).
 */
std::string ResolvedUpgradeUrl(const ClientCompatDocument& doc, const std::string& fallback,
                               const std::string& platform);

/**
 * UpdateRequired when `local_version`'s core is below `min_client_version`'s (CompareSemverCore: a prerelease meets
 * the floor of its release, as the relay compares); SoftUpdateAvailable when it is below `latest_client_version` by
 * CompareReleaseVersions (a release candidate hears that its release is out). A floor that does not parse as a version
 * is no floor (fail open: a typo in the relay's config must not lock every app out; ClientCompatFloorUnreadable says
 * so, for the log).
 */
CompatUiAction DecideCompatUiAction(std::string_view local_version, const ClientCompatDocument& doc);
/** `min_client_version` is set but is not a version: DecideCompatUiAction ignores it (log it). */
bool ClientCompatFloorUnreadable(const ClientCompatDocument& doc);
/**
 * pp-node (A2): `node_version`'s core is below the document's `min_node_version` (CompareSemverCore, as the relay
 * compares). False when there is no floor, or when either does not parse (not judged, as the relay does not).
 */
bool NodeBelowFloor(std::string_view node_version, const ClientCompatDocument& doc);
/**
 * As above, but UpdateRequired whatever the document says when `relay_requires_upgrade`: the org's relay answered
 * `426 client_upgrade_required` (the relay refusing us is the authority; its document may not raise the floor yet).
 */
CompatUiAction DecideCompatUiAction(std::string_view local_version, const ClientCompatDocument& doc,
                                    bool relay_requires_upgrade);

struct ClientCompatCacheEntry {
  int64_t fetched_at_unix = 0;
  ClientCompatDocument document;
};

Roe<ClientCompatCacheEntry> LoadClientCompatCache(const std::string& profile_data_dir);
Roe<void> SaveClientCompatCache(const std::string& profile_data_dir, const ClientCompatCacheEntry& entry);
bool ClientCompatCacheFresh(const ClientCompatCacheEntry& entry, int64_t now_unix,
                            int64_t ttl_seconds = kClientCompatCacheTtlSeconds);

class IClientCompatClient {
public:
  virtual ~IClientCompatClient() = default;
  virtual Roe<ClientCompatDocument> Fetch() = 0;
};

/** How a check reads the cache: CacheFirst takes a fresh one as it is; Forced asks the relay first. */
enum class CompatCheck { CacheFirst, Forced };

/** What one check found: the document to apply (none: fail open), when it was fetched, and why there is no fresh one. */
struct ClientCompatResolution {
  std::optional<ClientCompatDocument> document;
  /** Unix seconds the document was fetched (now when this check fetched it); 0 without a document. */
  int64_t fetched_at_unix = 0;
  /** This check fetched it (a cached copy otherwise). */
  bool fetched = false;
  std::string error;
};

/**
 * One client-compat check (blocks: run it off the UI thread): a fresh cache as it is (CacheFirst), else the relay,
 * cached on success; a failed fetch falls back to any cached copy (fail open), Forced included.
 */
ClientCompatResolution ResolveClientCompat(IClientCompatClient* client, const std::string& profile_data_dir,
                                           int64_t now_unix, CompatCheck mode);

/** The app came to the foreground: a forced check is due when the document held was fetched 15 minutes ago or more. */
bool ClientCompatRecheckDue(int64_t fetched_at_unix, int64_t now_unix,
                            int64_t recheck_seconds = kClientCompatRecheckSeconds);

} // namespace pbr
