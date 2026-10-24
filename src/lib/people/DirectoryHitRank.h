#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr {

/**
 * Orders directory search hits for a query: exact nickname / display-name matches first (case-insensitive),
 * then prefix matches, then the rest in the server's order. Stable, so ties keep the server's ranking.
 * (Kenneth 2026-10-23: searching a nickname must put that person first.)
 */
void RankDirectoryHits(std::vector<mesh::DirectoryHit>& hits, const std::string& query);

/** "account:…" / "relay:…" — a full id the directory can look up exactly instead of searching. */
bool LooksLikeExactPeopleId(const std::string& query);

} // namespace pbr
