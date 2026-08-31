#pragma once

#include "base/messaging/ThreadTypes.h"
#include "base/people/ContactTypes.h"
#include "base/people/ContactsStore.h"

#include <optional>
#include <string>
#include <unordered_map>

namespace pbr {

/** True when value looks like a Brief `relay:` route id. */
bool IsRelayUserIdValue(const std::string& value);

/** First non-empty RelayUser id on a contact. */
std::optional<std::string> RelayUserIdFromContact(const Contact& contact);

/** First non-empty RelayUser id on a directory hit (PrimaryRelayIdFromHit). */
std::optional<std::string> RelayUserIdFromDirectoryHit(const DirectoryHit& hit);

/**
 * Brief delivery route for a direct thread: contact RelayUser, else learned
 * account→relay map (inbound `sender_relay_id`).
 */
std::optional<std::string> ResolvePeerBriefRoute(const Thread& thread, ContactsStore& contacts,
                                                 const std::unordered_map<std::string, std::string>& account_to_relay);

} // namespace pbr
