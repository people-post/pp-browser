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
 * Whether a relay origin taken from a contact's account card may get a client: https only, no userinfo, and a host
 * that is a public name or a public IP literal (no loopback, private, link-local, CGNAT, multicast or reserved
 * literal, IPv4-mapped forms included; no `localhost`, `*.localhost`, `*.local`, `*.internal`, single-label or
 * numeric-looking names). Ports are not restricted. `lab_relaxed` (a lab build's own opt-in, DevCaBundleActive())
 * accepts any http(s) origin. Never applied to our own relays. A name that resolves inside is refused at connect
 * time (HttpRelayClient's `public_only`), not here.
 */
bool IsForeignRelayOriginAllowed(const std::string& origin, bool lab_relaxed = false);

/**
 * Our relays, preferred first (multi-relay R2): the backend's relay, then `extras` (config `relay.extra_base_urls`)
 * in their order. An extra that is not an absolute http(s) URL, names a relay already listed, or comes after the
 * fourth is left out. Each is trimmed of spaces and trailing `/`. An empty preferred: none at all.
 */
std::vector<std::string> OurRelayOrigins(const std::string& preferred, const std::vector<std::string>& extras);

} // namespace pbr
