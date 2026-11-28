#pragma once

#include "common/Error.h"

#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** `manifest.json`'s own schema (docs/contracts/DATA_LAYOUT.md). 1 was the per-backend manifest (`backend`, no
 * network): the old format, removed by the app and converted once by pp-node (profile-model PM003, PM007). */
inline constexpr int kProfileManifestSchemaVersion = 2;

/**
 * The world a profile belongs to (profile-model PM001): written when the profile is made, never changed. Accounts of
 * different networks never meet; a tester build never opens `main`.
 */
namespace profile_network {
inline constexpr std::string_view kMain = "main";
inline constexpr std::string_view kStaging = "staging";
inline constexpr std::string_view kDev = "dev";
inline constexpr std::string_view kTestCustom = "test-custom";
inline constexpr std::string_view kNode = "node";
} // namespace profile_network

bool IsKnownProfileNetwork(std::string_view network);

enum class ProfileManifestState {
  kCurrent,    // this schema, a known network
  kMissing,    // no manifest.json
  kUnreadable, // not JSON, no schema, or this schema without a known network: kept, never claimed
  kNewer,      // a later build's: kept, never claimed or written
  kOldFormat,  // schema 1 (a backend key, no network)
};

struct ProfileManifestRead {
  ProfileManifestState state = ProfileManifestState::kMissing;
  std::string network; // kCurrent only
};

ProfileManifestRead ReadProfileManifest(const std::string& profile_data_dir);

/** A new profile's manifest, written atomically; refused when the profile already has one. */
Roe<void> WriteNewProfileManifest(const std::string& profile_data_dir, std::string_view network);

/**
 * pp-node only (profile-model PM007): an old-format manifest written again as this schema on network `node`, so a
 * deployed node keeps the identity in its profile. Anything but the old format is refused.
 */
Roe<void> ConvertOldProfileManifestToNode(const std::string& profile_data_dir);

} // namespace pbr
