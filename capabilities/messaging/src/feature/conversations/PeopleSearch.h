#pragma once

#include "common/MeshAlias.h"
#include "mesh/types/DirectoryTypes.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr {

class ConversationsHub;

/**
 * The directory search both the assistant's "find someone" and the contacts page's "add contact" use:
 * a full id ("account:…" / "relay:…") is looked up exactly (not found = empty), anything else is searched,
 * our own identities are dropped, and exact nickname / display-name matches come first.
 */
Roe<std::vector<mesh::DirectoryHit>> SearchDirectoryPeople(ConversationsHub& hub, const std::string& query);

} // namespace pbr
