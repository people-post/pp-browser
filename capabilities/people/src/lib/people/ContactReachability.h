#pragma once

#include "lib/people/ContactTypes.h"

namespace pbr {

/**
 * Messaging / call picker eligibility (L4 / H003).
 * False if blocked or there is no Account / PeerId handle.
 * True for an Account ID with a configured relay client (the Account ID is its relay inbox, multi-relay MR001) or
 * whose keys are known (`account_keys_known`: a kept card — sends find its devices over Amp; a contact added from a
 * QR code has nothing else, #409), or a pasted / discovered multiaddr.
 */
bool IsContactReachableForMessaging(const Contact& contact, bool relay_configured, bool account_keys_known = false);

} // namespace pbr
