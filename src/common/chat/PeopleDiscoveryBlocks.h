#pragma once

#include "common/directory/DirectoryTypes.h"
#include "common/directory/IdentityTypes.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** Narrow contact view for people-discovery chat blocks (avoids people→messaging coupling). */
struct PeopleDiscoveryContactView {
  std::string id;
  std::string display_name;
  std::string server_nickname;
  std::vector<ContactId> ids;
};

/** Optional knobs when building people-discovery long_list blocks. */
struct PeopleDiscoveryBuildOptions {
  /** Identity values (account / relay / peer ids) already in local contacts. */
  std::unordered_set<std::string> known_local_identity_values;
  /** Local user identities — matching directory hits are omitted entirely. */
  std::unordered_set<std::string> self_identity_values;
  /** Cap visible directory / contact rows; remainder noted in footer. */
  size_t max_visible_items = 10;
};

/** account_id / relay_user_id / peer_id from local identity (non-empty only). */
std::unordered_set<std::string> SelfIdentityValuesFromLocal(const LocalIdentity& identity);

/** True when hit account/ids intersect `identities`. */
bool DirectoryHitMatchesIdentities(const DirectoryHit& hit,
                                   const std::unordered_set<std::string>& identities);

// Builds structured blocks JSON ({"blocks":[...]}) for people-discovery tool results.
std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<PeopleDiscoveryContactView>& contacts,
                                           const PeopleDiscoveryBuildOptions& options = {});

// If raw text is a directory-hits or contacts JSON array, build blocks JSON; otherwise empty.
std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json);

} // namespace pbr
