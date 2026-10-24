#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * Orders directory search hits for a query: exact nickname / display-name matches first (case-insensitive),
 * then prefix matches, then the rest in the server's order. Stable, so ties keep the server's ranking.
 * (Kenneth 2026-10-23: searching a nickname must put that person first.)
 */
void RankDirectoryHits(std::vector<mesh::DirectoryHit>& hits, const std::string& query);

/**
 * Canonical exact-lookup id for a query: trims whitespace; "account:…" / "relay:…" kept as is; a bare
 * 43-char base64url token (an account id without its prefix) becomes "account:" + token; else nullopt.
 */
std::optional<std::string> NormalizeExactPeopleId(std::string_view query);

} // namespace pbr
