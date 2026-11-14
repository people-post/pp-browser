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
 * The relay's host as a person reads it (the contact page): the authority of `origin` without scheme, path or a
 * default port, lower case (`https://www.brief.global/api/relay` → `www.brief.global`); empty when it is no URL.
 */
std::string RelayOriginHost(const std::string& origin);

/**
 * Our relays, preferred first (multi-relay R2): the backend's relay, then `extras` (config `relay.extra_base_urls`)
 * in their order. An extra that is not an absolute http(s) URL, names a relay already listed, or comes after the
 * fourth is left out. Each is trimmed of spaces and trailing `/`. An empty preferred: none at all.
 */
std::vector<std::string> OurRelayOrigins(const std::string& preferred, const std::vector<std::string>& extras);

/** Ours, preferred first, as a list given whole: OurRelayOrigins(list[0], the rest); empty: none. */
std::vector<std::string> BoundOurRelays(const std::vector<std::string>& list);

/** The longest relay base URL we list (MR004: the card's bound on an origin). */
inline constexpr size_t kMaxRelayOriginBytes = 128;

/** What is wrong with a relay the user typed (My relays' Add). */
enum class RelayOriginProblem { None, Empty, NotAUrl, NotHttps, HasPath, HasQuery, TooLong };

/** A relay the user typed: its base URL, or what is wrong with it. */
struct ParsedRelayOrigin {
  std::string base_url;
  RelayOriginProblem problem = RelayOriginProblem::None;
};

/**
 * A relay as the user types it (multi-relay R3, My relays): `https://host[:port]`, or that followed by `/api/relay`
 * (the relay's base URL, as a card lists it). Spaces and trailing `/` are trimmed, scheme and host lower-cased; no
 * user info, no other path, no query or fragment. The base URL is the origin and `/api/relay` (the deployment's
 * convention, BriefRelayBaseUrlFor), at most kMaxRelayOriginBytes. `http://` only when `allow_http` (developer
 * builds: the lab's plain relays; such a relay never goes on the card).
 */
ParsedRelayOrigin ParseRelayOrigin(const std::string& text, bool allow_http);

} // namespace pbr
