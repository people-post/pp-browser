#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * Numbering state for one diagnostics bundle: placeholders are numbered by first appearance, so the same
 * identifier maps to the same placeholder inside one bundle and nothing links two bundles.
 */
struct ScrubState {
  std::map<std::string, int> assigned;  // "kind|text" -> number
  std::map<std::string, int> counters;  // kind -> last number handed out
};

/**
 * Replace identifiers in one log line with numbered placeholders: acct_N, peer_N, relay_N, call_N, ip_N
 * (IPv4 / IPv6 / host:port / multiaddr, port dropped), path_N. Everything else stays byte-identical.
 * Pure; no I/O.
 */
std::string ScrubLine(std::string_view line, ScrubState& state);

/**
 * Independent last check on text that was already scrubbed (one or more lines). Does NOT reuse the scrub
 * patterns. Returns the name of the first detector that fires ("hex_run", "long_token", "email",
 * "abs_path", "known_identifier"), or nullopt. Known identifiers are searched as plain substrings after
 * dropping a leading scheme (account: relay: peer: call:); the bare id and its prefixes of 8+ chars match,
 * an id shorter than 8 chars is ignored.
 */
std::optional<std::string> SelfCheck(std::string_view scrubbed_text,
                                     const std::vector<std::string>& known_identifiers);

} // namespace pbr
