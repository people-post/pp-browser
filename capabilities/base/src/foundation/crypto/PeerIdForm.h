#pragma once

#include <string_view>

namespace pbr {

/**
 * `text` has the form of an ML-DSA PeerId (docs/architecture/MESH_IDENTITY.md): base58btc that decodes to exactly 34
 * bytes, `0x12 0x20 ‖ 32 bytes` (a SHA-256 multihash), and re-encodes to the same string — 46 characters, `Qm…`. Only
 * the form: no key is known to check it against. What a card's relay entry must name (multi-relay MR007).
 */
bool IsMlDsaPeerIdForm(std::string_view text);

} // namespace pbr
