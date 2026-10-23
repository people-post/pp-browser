#include "domain/people/DirectoryHitRank.h"

#include "common/Utilities.h"
#include "common/PbrCompat.h"

#include <algorithm>

namespace pbr {

namespace {

int MatchRank(const std::string& value, const std::string& query_lower) {
  if (value.empty()) {
    return 2;
  }
  const std::string lower = util::ToLowerAscii(util::Trim(value));
  if (lower == query_lower) {
    return 0;
  }
  if (lower.rfind(query_lower, 0) == 0) {
    return 1;
  }
  return 2;
}

} // namespace

void RankDirectoryHits(std::vector<mesh::DirectoryHit>& hits, const std::string& query) {
  const std::string query_lower = util::ToLowerAscii(util::Trim(query));
  if (query_lower.empty() || hits.size() < 2) {
    return;
  }
  std::stable_sort(hits.begin(), hits.end(), [&](const mesh::DirectoryHit& a, const mesh::DirectoryHit& b) {
    const int ra = std::min(MatchRank(a.nickname, query_lower), MatchRank(a.display_name, query_lower));
    const int rb = std::min(MatchRank(b.nickname, query_lower), MatchRank(b.display_name, query_lower));
    return ra < rb;
  });
}

bool LooksLikeExactPeopleId(const std::string& query) {
  const std::string q = util::Trim(query);
  return q.rfind("account:", 0) == 0 || q.rfind("relay:", 0) == 0;
}

} // namespace pbr
