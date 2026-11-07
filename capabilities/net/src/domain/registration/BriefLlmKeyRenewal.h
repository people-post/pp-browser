#pragma once

#include "lib/net/BriefGuestLlmClient.h"
#include "common/directory/IdentityTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What renewing the Brief LLM key after an HTTP 401 reads and writes (B183): the stored identity, its save, and the
 * guest mint. The app passes the identity store and MintBriefGuestLlmKey; tests pass fakes.
 */
struct BriefLlmKeyStore {
  std::function<std::optional<LocalIdentity>()> get;
  std::function<Roe<void>(const LocalIdentity& identity)> save;
  std::function<Roe<BriefGuestLlmMintResult>(const std::string& llm_base_url)> mint;
};

struct BriefLlmKeyRenewal {
  std::string key;     // the Bearer to ask again with
  bool reused = false; // the stored key had already changed: nothing was minted
};

/** The Bearer stored now: registered, else guest ("" when none, or the identity is unreadable). */
std::string StoredBriefLlmKey(const BriefLlmKeyStore& store);

/**
 * The gateway refused `rejected`. When the stored key is another one, that one (reused). Else mint a guest key at
 * `llm_base_url` and store it beside the registered key, which is kept until a retry with the guest key is answered
 * (CommitBriefLlmKey). One read-modify-write after the mint: the stored key is never empty meanwhile.
 */
Roe<BriefLlmKeyRenewal> RenewBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected,
                                         const std::string& llm_base_url);

/**
 * A retry with the renewed key was answered: drop the registered key if it is still `rejected` (one
 * read-modify-write; nothing is written when there is nothing to drop). Never re-registers.
 */
Roe<void> CommitBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected);

} // namespace pbr
