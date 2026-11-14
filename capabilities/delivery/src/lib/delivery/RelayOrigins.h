#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** At most this many relays of ours, the preferred included (multi-relay MR004, as the card's list). */
inline constexpr size_t kMaxOurRelays = 4;

/**
 * A relay origin as we compare it: scheme and host without case, `:443` on https (`:80` on http) as no port, no
 * trailing `/`. Two origins with the same key are the same relay (a card's, a config's, the backend's).
 */
std::string RelayOriginKey(const std::string& origin);

/** Both name the same relay (RelayOriginKey). */
bool SameRelay(const std::string& a, const std::string& b);

/**
 * Our relays, preferred first (multi-relay R2): the backend's relay, then `extras` (config `relay.extra_base_urls`)
 * in their order. An extra that is not an absolute http(s) URL, names a relay already listed, or comes after the
 * fourth is left out. Each is trimmed of spaces and trailing `/`. An empty preferred: none at all.
 */
std::vector<std::string> OurRelayOrigins(const std::string& preferred, const std::vector<std::string>& extras);

} // namespace pbr
