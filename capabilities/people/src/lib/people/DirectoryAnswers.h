#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** One directory's answer to a name search (multi-relay R2: one of our relays): its hits, or why it gave none. */
struct DirectoryAnswer {
  /** The directory's base URL, to say which could not be reached. */
  std::string origin;
  /** None: no answer (a failure, a timeout). */
  std::optional<std::vector<mesh::DirectoryHit>> hits;
  std::string error;
};

/**
 * The hit carries its account's keys and they prove themselves (VerifyKeyCard: the signing key hashes to the Account
 * ID, the KEM key carries the account's binding). A hit with no account or a key missing does not, nor one whose keys
 * need a newer app (`keys_update_needed`).
 */
bool DirectoryHitKeysVerify(const mesh::DirectoryHit& hit);

/**
 * One list from several directories' answers, the first one's (the preferred relay's) first, then each next one's
 * accounts not seen yet, each in its directory's order. An account (its Account ID) is listed once; when several
 * answered for it, its entry is the one whose keys verify over one whose do not, else the earlier directory's (an
 * entry's endpoint times are its relay's unsigned word and decide nothing). A hit with no Account ID is kept once per
 * hit id. Directories that gave no answer add nothing.
 */
std::vector<mesh::DirectoryHit> MergeDirectoryAnswers(const std::vector<DirectoryAnswer>& answers);

} // namespace pbr
