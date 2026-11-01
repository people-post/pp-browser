#pragma once

#include "lib/people/ContactTypes.h"

namespace pbr {

/**
 * Messaging / call picker eligibility (L4 / H003).
 * False if blocked or there is no Account / RelayUser / PeerId handle.
 * True when relay id + configured relay client, or a pasted / discovered multiaddr, or an Account ID whose
 * keys are known (`account_keys_known`: a kept card — sends find its devices over Amp or its relay route in
 * the directory; a contact added from a QR code has nothing else, #409).
 */
bool IsContactReachableForMessaging(const Contact& contact, bool relay_configured, bool account_keys_known = false);

} // namespace pbr
