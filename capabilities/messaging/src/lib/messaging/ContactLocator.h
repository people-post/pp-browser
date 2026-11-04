#pragma once

#include "common/Error.h"

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

  bool operator==(const ContactLocator&) const = default;
};

/** `pp-browser-contact-v1:<base64url-unpadded(JSON)>`. */
std::string EncodeContactLocator(const ContactLocator& locator);
/** The first locator in `text` (an invite line may wrap it in words); an error when there is none. */
Roe<ContactLocator> FindContactLocator(const std::string& text);

} // namespace pbr
