#pragma once

#include "base/people/ContactTypes.h"

namespace pbr {

class PeerSessionManager;

/** True when contact has a relay user id or PeerId identity (not blocked). */
bool ContactHasDialIdentity(const Contact& contact);

/** Stack address book says PeerId is dialable (direct or circuit-backed). */
bool IsContactStackDialable(const Contact& contact, const PeerSessionManager* sessions);

/**
 * Messaging / call picker eligibility (L4 / H003).
 * Relay id + configured relay client, pasted multiaddr, or stack dialable PeerId.
 */
bool IsContactReachableForMessaging(const Contact& contact, const PeerSessionManager* sessions,
                                    bool relay_configured);

} // namespace pbr
