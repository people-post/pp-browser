#pragma once

#include <string>

namespace pbr {

/**
 * The locale key for a call-media failure the transport reported as technical text
 * (`amp call-media: call dropped [leg: …]`, `peer not reached [...]`, …), or
 * nullptr when the text is already user copy (a feature's Localized() line). The raw text belongs in the
 * log only; the user sees a plain sentence (B89, 2026-10-23).
 */
const char* CallMediaErrorUserKey(const std::string& technical);

/**
 * The failure is already told by the call screen itself (its "Call dropped" / "Couldn't reach them" page): the
 * notice layer says nothing more (#181, one prompt in one place).
 */
bool CallMediaErrorShownOnCallScreen(const std::string& technical);

/**
 * The text is one of the calls feature's own localized notices (CallSessionWorkflow / CallDevice lines), in the
 * current language: shown as is. Any other text that is not a stable code becomes the generic error (#178).
 */
bool CallNoticeIsLocalizedCopy(const std::string& text);

} // namespace pbr
