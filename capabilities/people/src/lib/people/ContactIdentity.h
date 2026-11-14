#pragma once

#include "lib/people/ContactTypes.h"
#include "lib/people/PeerCards.h"
#include "common/MeshAlias.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** True when value is a full Account ID (`account:…`, M002). */
bool IsAccountIdentityValue(const std::string& value);

/** First matching id of kind (primary preferred). */
std::string PrimaryIdOfKind(const Contact& contact, mesh::ContactIdKind kind);

/** The id every screen seeds a contact's letter-avatar colour with: account, else peer, else the contact id. */
std::string ContactAvatarStableId(const Contact& contact);

std::optional<std::string> ContactAccountId(const Contact& contact);

/** The Account ID on a directory hit (its `account_id`, else its primary Account id). */
std::optional<std::string> PrimaryAccountIdFromHit(const mesh::DirectoryHit& hit);

/** Where a contact receives mail (multi-relay): its relays, preferred first, and whether a card said so. */
struct ContactRelayList {
  std::vector<std::string> origins;
  /** The same relays with the PeerId the card names for each (MR007; a locator's relay: none). */
  std::vector<AccountCardRelay> relays;
  /** The list is its account's newest kept card's (MR002); else the QR locator's origin, or nothing known. */
  bool from_card = false;
};

/**
 * A contact's relays: the newest account card kept for its account (`kept`, KeptRelays) once one is — its list even
 * when empty (the mesh only) — else the origin its QR locator gave (`locator_relay`, multi-relay R4), else none.
 */
ContactRelayList ContactRelays(const Contact& contact, const std::optional<PeerCardRecord>& kept);

} // namespace pbr
