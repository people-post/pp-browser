#pragma once

#include "lib/people/PeerCards.h"
#include "lib/people/PeerKemKeyStore.h"
#include "lib/people/PeerSigningKeyStore.h"

#include "common/Error.h"
#include "common/Module.h"
#include "mesh/types/IDirectoryClient.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The source our own card is kept under when it came back from our relays' directories (OwnCards' recovery): checked
 * by OwnCards against our own account keys first, so it passes the own-account guard (CheckNotOwn).
 */
inline constexpr const char kOwnCardRecoveredSource[] = "recovered";

/**
 * Where an account's signing and KEM keys come from (docs/contracts/ACCOUNT_CARD.md#resolving-keys): the
 * cards this device has checked and kept, else the directory when one is configured — its answer kept
 * as a card too. Every key goes through the key stores, which refuse anything that does not prove
 * itself against the Account ID; no source is trusted for more than carrying the bytes.
 */
class AccountKeyResolver : public Module {
public:
  AccountKeyResolver(PeerSigningKeyStore& signing_keys, PeerKemKeyStore& kem_keys, PeerCards& cards,
                     mesh::IDirectoryClient& directory);

  /**
   * The profile's own Account ID (any thread; empty: not known). A card of our own account is kept only from our own
   * devices (sources `self`, `sibling`: OwnCards) or as OwnCards got it back from our relays, checked against our own
   * account keys (kOwnCardRecoveredSource): one carried back by a contact, a directory or a relay any other way is
   * refused, so it never relabels our record (the profile.db ladder and the device list read it).
   */
  void SetOwnAccount(std::function<std::string()> own_account) { own_account_ = std::move(own_account); }
  /** Fills the key stores from every kept card (at unlock). */
  void LoadKeptCards();
  /** What is kept for `account_id` (no lookup). */
  Roe<std::optional<PeerCardRecord>> Kept(const std::string& account_id) { return cards_.Get(account_id); }
  /**
   * The issue time an account card of `account_id` must reach (PeerCards::Floor; 0 when none). Unreadable:
   * none does (a failed read must not let an older card in).
   */
  int64_t CardFloor(const std::string& account_id) {
    auto floor = cards_.Floor(account_id);
    return floor ? *floor : std::numeric_limits<int64_t>::max();
  }
  /** The keys of `account_id`: a kept card, else the directory's entry, kept as a card. */
  Roe<PeerCardRecord> Resolve(const std::string& account_id);
  /** Keeps an account card that came another way (a contact card, a device, a locator's fetch). */
  Roe<PeerCardRecord> AddAccountCard(const AccountCard& card, int64_t now_ms, const std::string& source);
  /** Keeps a key card that came another way (a first message's sender_keys, a contact card). */
  Roe<PeerCardRecord> AddKeyCard(const KeyCard& card, const std::string& source);
  /**
   * Keeps a card as a message carries it (CarriedCardJson): an account card when signed as one, else a
   * key card. With `account_id`, a card for any other account is refused.
   */
  Roe<PeerCardRecord> AddCarriedCard(const Object& json, int64_t now_ms, const std::string& source,
                                     const std::string& account_id = {});
  /**
   * Keeps the keys a directory entry carries for its account (search hit, contact sync). An entry whose keys need a
   * newer app (`keys_update_needed`) is refused as `AppError::NewerFormat`, not as bad keys.
   */
  Roe<PeerCardRecord> AddDirectoryHit(const mesh::DirectoryHit& hit);

  IPeerSigningKeyResolver& Signing() { return signing_; }
  /** Signing keys from kept cards only — never a lookup (for checks on an I/O or worker thread). */
  IPeerSigningKeyResolver& KeptSigning() { return kept_signing_; }
  IPeerKemKeyResolver& Kem() { return kem_; }

private:
  /** Refused: a card of our own account from anyone but our own devices. */
  Roe<void> CheckNotOwn(const std::string& account_id, const std::string& source) const;

  std::function<std::string()> own_account_;

  class SigningView : public IPeerSigningKeyResolver {
  public:
    explicit SigningView(AccountKeyResolver& owner) : owner_(owner) {}
    Roe<PeerSigningKeyRecord> Resolve(const std::string& kind, const std::string& value) override;

  private:
    AccountKeyResolver& owner_;
  };
  class KeptSigningView : public IPeerSigningKeyResolver {
  public:
    explicit KeptSigningView(AccountKeyResolver& owner) : owner_(owner) {}
    Roe<PeerSigningKeyRecord> Resolve(const std::string& kind, const std::string& value) override;

  private:
    AccountKeyResolver& owner_;
  };
  class KemView : public IPeerKemKeyResolver {
  public:
    explicit KemView(AccountKeyResolver& owner) : owner_(owner) {}
    Roe<PeerKemKeyRecord> Resolve(const std::string& kind, const std::string& value) override;

  private:
    AccountKeyResolver& owner_;
  };

  static bool IsAccountKind(const std::string& kind);
  /** Puts a kept card's keys into the key stores. */
  void Remember(const PeerCardRecord& record);

  PeerSigningKeyStore& signing_keys_;
  PeerKemKeyStore& kem_keys_;
  PeerCards& cards_;
  mesh::IDirectoryClient& directory_;
  SigningView signing_{*this};
  KeptSigningView kept_signing_{*this};
  KemView kem_{*this};
};

} // namespace pbr
