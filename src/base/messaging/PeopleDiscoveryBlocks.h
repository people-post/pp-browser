#pragma once

// Compatibility shim — prefer `#include "common/PeopleDiscoveryBlocks.h"`.
#include "common/PeopleDiscoveryBlocks.h"
#include "base/people/ContactTypes.h"

#include <vector>

namespace pbr {

inline PeopleDiscoveryContactView ToPeopleDiscoveryContactView(const Contact& contact) {
  return PeopleDiscoveryContactView{
      .id = contact.id,
      .display_name = contact.display_name,
      .server_nickname = contact.server_nickname,
      .ids = contact.ids,
  };
}

inline std::vector<PeopleDiscoveryContactView> ToPeopleDiscoveryContactViews(
    const std::vector<Contact>& contacts) {
  std::vector<PeopleDiscoveryContactView> views;
  views.reserve(contacts.size());
  for (const Contact& contact : contacts) {
    views.push_back(ToPeopleDiscoveryContactView(contact));
  }
  return views;
}

inline std::string BuildPeopleDiscoveryBlocksJson(const std::vector<DirectoryHit>& directory_hits,
                                                  const std::vector<Contact>& contacts) {
  return BuildPeopleDiscoveryBlocksJson(directory_hits, ToPeopleDiscoveryContactViews(contacts));
}

} // namespace pbr
