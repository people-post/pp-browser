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
  /**
   * One read-modify-write under the identity store's lock (IdentityStore::Modify): `change` edits the identity as
   * stored now and returns whether to save it. Never a whole identity read earlier: a writer on another thread (the
   * registration renewal) keeps its fields. `change` must not call back into the store.
   */
  std::function<Roe<void>(const std::function<bool(LocalIdentity& identity)>& change)> modify;
  std::function<Roe<BriefGuestLlmMintResult>(const std::string& llm_base_url)> mint;
};

struct BriefLlmKeyRenewal {
  std::string key;     // the Bearer to ask again with
  bool reused = false; // the stored key had already changed: nothing was minted
};

/** The Bearer stored now: registered, else guest ("" when none, or the identity is unreadable). */
std::string StoredBriefLlmKey(const BriefLlmKeyStore& store);

/**
 * Stores `key` as the guest key unless a key is stored already (registered or guest: kept, and returned), in one
 * modify. `expected_account_id`: the account `key` was minted for; another account stored now (the profile changed
 * meanwhile), or an empty `expected_account_id`, is refused and nothing is written. Returns the Bearer stored after it.
 */
Roe<std::string> StoreBriefGuestKeyIfNone(const BriefLlmKeyStore& store, const SecretText& key,
                                          const std::string& expected_account_id);

/**
 * The gateway refused `rejected`. When the stored key is another one, that one (reused); when the refused one is the
 * registered key and a different guest key is stored beside it, the guest key (reused, nothing minted). Else mint a guest key at
 * `llm_base_url` and store it beside the registered key, which is kept until a retry with the guest key is answered
 * (CommitBriefLlmKey). One modify after the mint: the stored key is never empty meanwhile; a key other than
 * `rejected` stored during the mint is kept (reused), and an identity of another account then is refused.
 */
Roe<BriefLlmKeyRenewal> RenewBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected,
                                         const std::string& llm_base_url);

/**
 * A retry with the renewed key was answered: drop the registered key if it is still `rejected` (one
 * read-modify-write; nothing is written when there is nothing to drop). Never re-registers.
 */
Roe<void> CommitBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected);

} // namespace pbr
