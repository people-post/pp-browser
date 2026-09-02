#pragma once

#include "common/thread/ThreadTypes.h"

#include <cstdint>
#include <string>

namespace pbr {

/** Canonical opaque stream id for a 1:1 pair + channel + epoch (relay-agnostic). */
std::string BuildCanonicalRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b,
                                         ThreadChannel channel, uint32_t session_epoch);
/** Opaque group stream scope (D095). */
std::string BuildGroupRelayStreamKey(const std::string& group_id, uint32_t session_epoch);

} // namespace pbr
