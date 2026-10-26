#pragma once

#include "mesh/types/AddressDisclosure.h"
#include "lib/people/ContactTypes.h"

#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The address-disclosure policy for `audience` over the address book (projects/privacy T1): each
 * contact's device PeerIds and identity values (account ids) go to `contacts`, and to `friendly` when
 * marked Friendly — or only to `blocked` when Blocked. `own_devices`: our own account's other devices
 * (M021), as trusted as a Friendly contact.
 */
mesh::AddressDisclosurePolicy BuildAddressDisclosurePolicy(mesh::DirectAudience audience, const std::vector<Contact>& contacts,
                                                           const std::vector<std::string>& own_devices = {});

} // namespace pbr
