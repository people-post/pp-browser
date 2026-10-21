#pragma once

#include "common/thread/IThreadStore.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>

namespace pbr {

/**
 * This device's DM with a peer, for what belongs to the pair (a call's history): the private E2E DM
 * when there is one — the one the user chats in — else the public one, made when missing.
 */
Roe<std::string> ResolveDirectThreadForPeer(IThreadStore& store, const std::string& peer_identity,
                                            const std::string& contact_id, const std::string& title);

} // namespace pbr
