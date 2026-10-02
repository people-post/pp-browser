#pragma once

#include "common/privacy/AddressDisclosure.h"
#include "domain/people/ContactTypes.h"

#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The address-disclosure policy for `audience` over the address book (projects/privacy T1): each
 * contact's device PeerIds and identity values (account ids) go to `contacts`, and to `friendly` when
 * marked Friendly — or only to `blocked` when Blocked.
 */
AddressDisclosurePolicy BuildAddressDisclosurePolicy(DirectAudience audience, const std::vector<Contact>& contacts);

} // namespace pbr
