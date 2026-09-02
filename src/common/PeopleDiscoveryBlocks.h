#pragma once

#include "common/DirectoryTypes.h"

#include <string>
#include <vector>

namespace pbr {

/** Narrow contact view for people-discovery chat blocks (avoids people→messaging coupling). */
struct PeopleDiscoveryContactView {
  std::string id;
  std::string display_name;
  std::string server_nickname;
  std::vector<ContactId> ids;
};

// Builds structured blocks JSON ({"blocks":[...]}) for people-discovery tool results.
std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<PeopleDiscoveryContactView>& contacts);

// If raw text is a directory-hits or contacts JSON array, build blocks JSON; otherwise empty.
std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json);

} // namespace pbr
