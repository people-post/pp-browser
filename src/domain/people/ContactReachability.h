#pragma once

#include "domain/people/ContactTypes.h"

namespace pbr {

/**
 * Messaging / call picker eligibility (L4 / H003).
 * False if blocked or there is no Account / RelayUser / PeerId handle.
 * True when relay id + configured relay client, or a pasted / discovered multiaddr.
 */
bool IsContactReachableForMessaging(const Contact& contact, bool relay_configured);

} // namespace pbr
