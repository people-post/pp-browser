#pragma once

#include "common/directory/DirectoryTypes.h"

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
  /** Cap visible directory / contact rows; remainder noted in footer. */
  size_t max_visible_items = 10;
};

// Builds structured blocks JSON ({"blocks":[...]}) for people-discovery tool results.
std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<PeopleDiscoveryContactView>& contacts,
                                           const PeopleDiscoveryBuildOptions& options = {});

// If raw text is a directory-hits or contacts JSON array, build blocks JSON; otherwise empty.
std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json);

} // namespace pbr
