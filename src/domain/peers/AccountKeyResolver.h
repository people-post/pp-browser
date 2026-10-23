#pragma once

#include "lib/messaging/PeerCardStore.h"
#include "lib/messaging/PeerKemKeyStore.h"
#include "lib/messaging/PeerSigningKeyStore.h"

#include "common/Error.h"
#include "common/Module.h"
#include "mesh/types/IDirectoryClient.h"

#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * Where an account's signing and KEM keys come from (docs/contracts/ACCOUNT_CARD.md#resolving-keys): the
 * cards this device has checked and kept, else the directory when one is configured — its answer kept
 * as a card too. Every key goes through the key stores, which refuse anything that does not prove
 * itself against the Account ID; no source is trusted for more than carrying the bytes.
 */
class AccountKeyResolver : public Module {
public:
  AccountKeyResolver(PeerSigningKeyStore& signing_keys, PeerKemKeyStore& kem_keys, PeerCardStore& cards,
                     mesh::IDirectoryClient& directory);

  /** Fills the key stores from every kept card (at unlock). */
  void LoadKeptCards();
  /** The keys of `account_id`: a kept card, else the directory's entry, kept as a card. */
  Roe<PeerCardRecord> Resolve(const std::string& account_id);
  /** Keeps the keys a directory entry carries for its account (search hit, contact sync). */
  Roe<PeerCardRecord> AddDirectoryHit(const mesh::DirectoryHit& hit);

  IPeerSigningKeyResolver& Signing() { return signing_; }
  IPeerKemKeyResolver& Kem() { return kem_; }

private:
  class SigningView : public IPeerSigningKeyResolver {
  public:
    explicit SigningView(AccountKeyResolver& owner) : owner_(owner) {}
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
  PeerCardStore& cards_;
  mesh::IDirectoryClient& directory_;
  SigningView signing_{*this};
  KemView kem_{*this};
};

} // namespace pbr
