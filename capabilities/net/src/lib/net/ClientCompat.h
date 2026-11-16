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

inline constexpr int kClientCompatSchemaVersion = 1;
inline constexpr int64_t kClientCompatCacheTtlSeconds = 6 * 60 * 60;
/** The app in front again asks anew once the document it holds was fetched this long ago (a forced check). */
inline constexpr int64_t kClientCompatRecheckSeconds = 15 * 60;

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

CompatUiAction DecideCompatUiAction(std::string_view local_version, const ClientCompatDocument& doc);
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
