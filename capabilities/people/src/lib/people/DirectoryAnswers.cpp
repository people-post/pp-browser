#include "lib/people/DirectoryAnswers.h"

#include "lib/people/AccountCard.h"
#include "lib/people/ContactIdentity.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>

namespace pbr {

namespace {

/** How new an entry is: its newest device endpoint (a search hit carries no card). */
int64_t NewestEndpointAt(const mesh::DirectoryHit& hit) {
  int64_t newest = 0;
  for (const mesh::DirectoryEndpoint& endpoint : hit.endpoints) {
    newest = std::max(newest, endpoint.updated_at);
  }
  return newest;
}

/** One listed entry and whether its keys verify (asked only once another directory answered for it too). */
struct Listed {
  mesh::DirectoryHit hit;
  std::optional<bool> verifies;

  bool Verifies() {
    if (!verifies) {
      verifies = DirectoryHitKeysVerify(hit);
    }
    return *verifies;
  }
};

/** A later directory's entry replaces the listed one: its keys verify and the listed one's do not, else it is newer. */
bool Replaces(const mesh::DirectoryHit& later, bool later_verifies, Listed& listed) {
  if (later_verifies != listed.Verifies()) {
    return later_verifies;
  }
  return NewestEndpointAt(later) > NewestEndpointAt(listed.hit); // a tie keeps the earlier directory's
}

std::string MergeKey(const mesh::DirectoryHit& hit) {
  if (auto account = PrimaryAccountIdFromHit(hit)) {
    return *account;
  }
  return "hit:" + hit.hit_id;
}

} // namespace

bool DirectoryHitKeysVerify(const mesh::DirectoryHit& hit) {
  const auto account = PrimaryAccountIdFromHit(hit);
  if (!account || !hit.signing_public_key_b64 || !hit.kem_public_key_b64 || !hit.kem_binding_signature_b64) {
    return false;
  }
  return static_cast<bool>(VerifyKeyCard(KeyCard{.account_id = *account,
                                                 .signing_public_key_b64 = *hit.signing_public_key_b64,
                                                 .kem_public_key_b64 = *hit.kem_public_key_b64,
                                                 .kem_binding_signature_b64 = *hit.kem_binding_signature_b64}));
}

std::vector<mesh::DirectoryHit> MergeDirectoryAnswers(const std::vector<DirectoryAnswer>& answers) {
  std::vector<Listed> listed;
  std::unordered_map<std::string, size_t> at;
  for (const DirectoryAnswer& answer : answers) {
    if (!answer.hits) {
      continue;
    }
    for (const mesh::DirectoryHit& hit : *answer.hits) {
      const std::string key = MergeKey(hit);
      const auto known = at.find(key);
      if (known == at.end()) {
        at.emplace(key, listed.size());
        listed.push_back(Listed{.hit = hit, .verifies = std::nullopt});
        continue;
      }
      Listed& entry = listed[known->second];
      const bool verifies = DirectoryHitKeysVerify(hit);
      if (Replaces(hit, verifies, entry)) {
        entry = Listed{.hit = hit, .verifies = verifies};
      }
    }
  }
  std::vector<mesh::DirectoryHit> out;
  out.reserve(listed.size());
  for (Listed& entry : listed) {
    out.push_back(std::move(entry.hit));
  }
  return out;
}

} // namespace pbr
