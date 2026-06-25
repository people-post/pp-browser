#pragma once

#include "base/people/ContactTypes.h"

#include <string>
#include <vector>

namespace pbr {

// Builds structured blocks JSON ({"blocks":[...]}) for people-discovery tool results.
std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                           const std::vector<Contact>& contacts);

// If raw text is a directory-hits or contacts JSON array, build blocks JSON; otherwise empty.
std::string TryPeopleDiscoveryBlocksFromToolJson(const std::string& raw_json);

} // namespace pbr
