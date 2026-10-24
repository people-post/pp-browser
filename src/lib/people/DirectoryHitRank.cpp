#include "lib/people/DirectoryHitRank.h"

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
    if (ra != rb) {
      return ra < rb;
    }
    // Final tie-break: account id ascending, hits without one last, so the order is deterministic.
    const bool ha = a.account_id.has_value() && !a.account_id->empty();
    const bool hb = b.account_id.has_value() && !b.account_id->empty();
    if (ha != hb) {
      return ha;
    }
    return ha && *a.account_id < *b.account_id;
  });
}

std::optional<std::string> NormalizeExactPeopleId(std::string_view query) {
  const std::string q = util::Trim(std::string(query));
  if (q.rfind("account:", 0) == 0 || q.rfind("relay:", 0) == 0) {
    return q;
  }
  if (q.size() != 43) {
    return std::nullopt;
  }
  for (const char ch : q) {
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                    ch == '-';
    if (!ok) {
      return std::nullopt;
    }
  }
  return "account:" + q;
}

} // namespace pbr
