#include "feature/conversations/PeopleSearch.h"

#include "common/Utilities.h"
#include "common/chat/PeopleDiscoveryBlocks.h"
#include "lib/people/DirectoryHitRank.h"
#include "lib/people/PeopleLookupFailure.h"
#include "feature/conversations/ConversationsHub.h"

#include <unordered_set>

namespace pbr {

Roe<std::vector<mesh::DirectoryHit>> SearchDirectoryPeople(ConversationsHub& hub, const std::string& query) {
  if (!hub.HasDirectory()) {
    return Error("Directory not configured");
  }
  std::vector<mesh::DirectoryHit> hits;
  bool searched_exact = false;
  if (const std::optional<std::string> exact_id = NormalizeExactPeopleId(query)) {
    const std::string& id = *exact_id;
    auto hit = id.rfind("account:", 0) == 0 ? hub.Directory().LookupByAccount(id) : hub.Directory().LookupRelayUser(id);
    if (hit) {
      hits.push_back(*hit);
      searched_exact = true;
    } else if (ClassifyPeopleLookupError(hit.error()) != PeopleLookupFailure::NotFound) {
      return hit.error();
    } else {
      // A bare 43-char token that is not an account may be a nickname of that shape: fall through to search.
      // A prefixed id ("account:" / "relay:") that is not found stays empty.
      const size_t start = query.find_first_not_of(" \t\r\n");
      const std::string_view trimmed = start == std::string::npos ? std::string_view() : std::string_view(query).substr(start);
      searched_exact = trimmed.rfind("account:", 0) == 0 || trimmed.rfind("relay:", 0) == 0;
    }
  }
  if (!searched_exact && hits.empty()) {
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
