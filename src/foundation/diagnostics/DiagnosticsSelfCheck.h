#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * The last gate before diagnostics leave the device: what is sent is already made of shareable lines
 * (common/ShareableLine.h), which cannot carry free text or raw ids, so a hit here means a line got past
 * that rule. Returns the name of the first detector that fires ("known_identifier", "hex_run", "long_token",
 * "email", "abs_path"), or nullopt. Known identifiers are searched as plain substrings after dropping a
 * leading scheme (account: relay: peer: call:); the bare id and its prefixes of 8+ chars match, an id
 * shorter than 8 chars is ignored.
 */
std::optional<std::string> SelfCheck(std::string_view text, const std::vector<std::string>& known_identifiers);

} // namespace pbr
