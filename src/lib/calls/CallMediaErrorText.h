#pragma once

#include <string>

namespace pbr {

/**
 * The locale key for a call-media failure the transport reported as technical text
 * (`amp call-media: no path within the reconnect window`, `peer not reached [...]`, …), or
 * nullptr when the text is already user copy (a feature's Tr() line). The raw text belongs in the
 * log only; the user sees a plain sentence (B89, 2026-10-23).
 */
const char* CallMediaErrorUserKey(const std::string& technical);

} // namespace pbr
