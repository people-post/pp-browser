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

Roe<std::string> StoreBriefGuestKeyIfNone(const BriefLlmKeyStore& store, const SecretText& key,
                                          const std::string& expected_account_id) {
  if (!store.modify) {
    return Error("Brief LLM key store is not wired");
  }
  if (expected_account_id.empty()) {
    return Error("Brief key not stored: no account to store it for");
  }
  bool other_account = false;
  std::string stored;
  auto changed = store.modify([&](LocalIdentity& identity) {
    if (identity.account_id != expected_account_id) {
      other_account = true;
      return false;
    }
    if (std::string current = Resolved(identity); !current.empty()) {
      stored = std::move(current); // a key came meanwhile (a renewal, a 401 heal): it stays
      return false;
    }
    identity.brief_llm_guest_api_key = key;
    stored = key.Reveal();
    return true;
  });
  if (!changed) {
    return changed.error();
  }
  if (other_account) {
    return Error("Brief key not stored: the identity is another account now");
  }
  return stored;
}

Roe<BriefLlmKeyRenewal> RenewBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected,
                                         const std::string& llm_base_url) {
  if (!store.get || !store.modify || !store.mint) {
    return Error("Brief LLM key store is not wired");
  }
  const std::optional<LocalIdentity> before = store.get();
  if (!before || before->account_id.empty()) {
    return Error("Messaging is not ready"); // no account to mint for (nor to check the write against)
  }
  if (std::string current = Resolved(*before); !current.empty() && !(SecretText(current) == SecretText(rejected))) {
    return BriefLlmKeyRenewal{.key = std::move(current), .reused = true}; // another turn already replaced it
  }
  auto minted = store.mint(llm_base_url);
  if (!minted) {
    return minted.error();
  }
  // One modify, on the identity as stored now (the mint took a while): only the guest key field is written.
  bool other_account = false;
  std::optional<std::string> kept;
  auto changed = store.modify([&](LocalIdentity& identity) {
    if (identity.account_id != before->account_id) {
      other_account = true;
      return false;
    }
    // A key stored during the mint (another turn, the unlock's mint) stays and is reused: not overwritten.
    const SecretText refused(rejected);
    const auto fresh = [&](const SecretText& now, const SecretText& was) {
      return !now.empty() && !(now == was) && !(now == refused);
    };
    if (fresh(identity.brief_llm_api_key, before->brief_llm_api_key)) {
      kept = identity.brief_llm_api_key.Reveal();
      return false;
    }
    if (fresh(identity.brief_llm_guest_api_key, before->brief_llm_guest_api_key)) {
      kept = identity.brief_llm_guest_api_key.Reveal();
      return false;
    }
    identity.brief_llm_guest_api_key = minted->llm_api_key; // replaces a refused guest key; a registered one stays
    return true;
  });
  if (!changed) {
    return changed.error();
  }
  if (other_account) {
    return Error("Brief key not stored: the identity is another account now");
  }
  if (kept) {
    return BriefLlmKeyRenewal{.key = std::move(*kept), .reused = true};
  }
  return BriefLlmKeyRenewal{.key = minted->llm_api_key.Reveal(), .reused = false};
}

Roe<void> CommitBriefLlmKey(const BriefLlmKeyStore& store, const std::string& rejected) {
  if (!store.modify) {
    return Error("Brief LLM key store is not wired");
  }
  if (rejected.empty()) {
    return {};
  }
  return store.modify([&](LocalIdentity& identity) {
    if (identity.brief_llm_api_key.empty() || !(identity.brief_llm_api_key == SecretText(rejected))) {
      return false;
    }
    identity.brief_llm_api_key.clear();
    return true;
  });
}

} // namespace pbr
