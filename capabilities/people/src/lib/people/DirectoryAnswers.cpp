#include "lib/people/DirectoryAnswers.h"

#include "lib/people/AccountCard.h"
#include "lib/people/ContactIdentity.h"

#include <unordered_map>

namespace pbr {

namespace {

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

/**
 * A later directory's entry replaces the listed one only when its keys verify and the listed one's do not. Otherwise
 * the earlier directory's (preferred first) stays: what else an entry says (its endpoints' times) is the relay's
 * unsigned word, so a later relay cannot override the name, photo or addresses by claiming newer ones.
 */
bool Replaces(bool later_verifies, Listed& listed) {
  return later_verifies && !listed.Verifies();
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
  if (!account || hit.keys_update_needed || !hit.signing_public_key_b64 || !hit.kem_public_key_b64 ||
      !hit.kem_binding_b64) {
    return false;
  }
  return static_cast<bool>(VerifyKeyCard(KeyCard{.account_id = *account,
                                                 .signing_public_key_b64 = *hit.signing_public_key_b64,
                                                 .kem_public_key_b64 = *hit.kem_public_key_b64,
                                                 .kem_binding_b64 = *hit.kem_binding_b64}));
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
      if (Replaces(verifies, entry)) {
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
