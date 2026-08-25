#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
  std::string message;
  std::optional<ClientCompatSupport> support;
};

enum class CompatUiAction {
  None = 0,
  SoftUpdateAvailable,
  UpdateRequired,
};

Roe<ClientCompatDocument> ParseClientCompatDocument(std::string_view json_text);

/** Resolve upgrade_url (fallback to kDefaultUpgradeUrl when empty). */
std::string ResolvedUpgradeUrl(const ClientCompatDocument& doc);

CompatUiAction DecideCompatUiAction(std::string_view local_version, const ClientCompatDocument& doc);

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

} // namespace pbr
