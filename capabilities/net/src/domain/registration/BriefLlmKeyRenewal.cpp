#include "domain/registration/BriefLlmKeyRenewal.h"

#include "common/Secret.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string Resolved(const LocalIdentity& identity) {
  return ResolveBriefLlmApiKey(identity.brief_llm_api_key.Reveal(), identity.brief_llm_guest_api_key.Reveal());
}

} // namespace

std::string StoredBriefLlmKey(const BriefLlmKeyStore& store) {
  const std::optional<LocalIdentity> identity = store.get ? store.get() : std::nullopt;
  return identity ? Resolved(*identity) : std::string();
}

Roe<BriefLlmKeyRenewal> RenewBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected,
                                         const std::string& llm_base_url) {
  if (!store.get || !store.save || !store.mint) {
    return Error("Brief LLM key store is not wired");
  }
  if (!store.get()) {
    return Error("Messaging is not ready");
  }
  if (std::string current = StoredBriefLlmKey(store); !current.empty() && !(SecretText(current) == SecretText(rejected))) {
    return BriefLlmKeyRenewal{.key = std::move(current), .reused = true}; // another turn already replaced it
  }
  auto minted = store.mint(llm_base_url);
  if (!minted) {
    return minted.error();
  }
  std::optional<LocalIdentity> identity = store.get(); // read again: the mint took a while
  if (!identity) {
    return Error("Messaging is not ready");
  }
  identity->brief_llm_guest_api_key = minted->llm_api_key; // replaces a refused guest key; a registered one stays
  if (auto saved = store.save(*identity); !saved) {
    return saved.error();
  }
  return BriefLlmKeyRenewal{.key = minted->llm_api_key.Reveal(), .reused = false};
}

Roe<void> CommitBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected) {
  if (!store.get || !store.save) {
    return Error("Brief LLM key store is not wired");
  }
  std::optional<LocalIdentity> identity = store.get();
  if (!identity) {
    return Error("Messaging is not ready");
  }
  if (rejected.empty() || identity->brief_llm_api_key.empty() || !(identity->brief_llm_api_key == SecretText(rejected))) {
    return {};
  }
  identity->brief_llm_api_key.clear();
  return store.save(*identity);
}

} // namespace pbr
