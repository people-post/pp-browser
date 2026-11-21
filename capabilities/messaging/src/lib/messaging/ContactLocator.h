#pragma once

#include "common/Error.h"
#include "lib/people/AccountCard.h"
#include "lib/people/ContactTypes.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char* kContactLocatorPrefix = "pp-browser-contact-v1:";

/**
 * Who to look for and where (docs/contracts/ACCOUNT_CARD.md#locator-qr-code-invite-link): what fits in a QR
 * code or an invite line. Unsigned — the Account ID is what the person showed; the card fetched for it
 * decides everything else, and the devices are hints.
 */
struct ContactLocator {
  std::string account_id;
  std::string nickname;
  std::vector<std::string> devices;
  /**
   * The account's preferred relay (multi-relay R4, MR008 as SIGNED_FORMATS § 3.3 amends it): its address, an
   * `https://` origin of at most 128 bytes (the card's bound) and the relay id its certificate proves, so a contact
   * added this way can write before it has the card; empty: none. JSON `relay: {"origin", "relay_id": "relay:…"}`.
   * Optional: a locator without it reads as before, and an app that does not know it ignores it.
   */
  RelayAddress relay;

  bool operator==(const ContactLocator&) const = default;
};

/**
 * `pp-browser-contact-v1:<base64url-unpadded(JSON)>`; a `relay` outside MR004's bounds, or naming no relay id, is left
 * out.
 */
std::string EncodeContactLocator(const ContactLocator& locator);
/**
 * What a user hands to someone else to find this contact: their Account ID, their OWN nickname (`remote.nickname`,
 * never the private remark `local.display_name` the user keeps) and the device ids known for them. Empty when the
 * contact has no Account ID.
 */
std::string ContactShareLocator(const Contact& contact);
/**
 * The first locator in `text` (an invite line may wrap it in words); an error when there is none. A `relay` outside
 * MR004's bounds, or without a relay id of the `relay:` form (MR008; a `peer_id` is not read), is dropped (the locator
 * still holds).
 */
Roe<ContactLocator> FindContactLocator(const std::string& text);

} // namespace pbr
