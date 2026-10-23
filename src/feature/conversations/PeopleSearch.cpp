#include "feature/conversations/PeopleSearch.h"

#include "common/Utilities.h"
#include "common/chat/PeopleDiscoveryBlocks.h"
#include "domain/people/DirectoryHitRank.h"
#include "feature/conversations/ConversationsHub.h"

#include <unordered_set>

namespace pbr {

Roe<std::vector<mesh::DirectoryHit>> SearchDirectoryPeople(ConversationsHub& hub, const std::string& query) {
  if (!hub.HasDirectory()) {
    return Error("Directory not configured");
  }
  std::vector<mesh::DirectoryHit> hits;
  if (LooksLikeExactPeopleId(query)) {
    const std::string id = util::Trim(query);
    auto hit = id.rfind("account:", 0) == 0 ? hub.Directory().LookupByAccount(id) : hub.Directory().LookupRelayUser(id);
    if (hit) {
      hits.push_back(*hit);
    } else if (hit.error().message.find("not found") == std::string::npos) {
      return hit.error();
    }
  } else {
    auto found = hub.Directory().SearchPeople(query);
    if (!found) {
      return found.error();
    }
    hits = std::move(*found);
    RankDirectoryHits(hits, query);
  }

  std::unordered_set<std::string> self_ids;
  if (auto identity = hub.Identity().Get()) {
    self_ids = SelfIdentityValuesFromLocal(*identity);
  }
  if (self_ids.empty()) {
    return hits;
  }
  std::vector<mesh::DirectoryHit> filtered;
  filtered.reserve(hits.size());
  for (mesh::DirectoryHit& hit : hits) {
    if (DirectoryHitMatchesIdentities(hit, self_ids)) {
      continue;
    }
    filtered.push_back(std::move(hit));
  }
  return filtered;
}

} // namespace pbr
