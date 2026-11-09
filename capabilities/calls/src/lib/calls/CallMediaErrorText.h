#pragma once

#include "lib/calls/CallMediaError.h"

#include <string>

namespace pbr {

/**
 * The locale key for a call-media failure reported as technical text only (`call-media: peer not reached [...]`,
 * `call-media migrate: timed out`, … — the hop path and the 1:1 connect sequence carry no kind), or nullptr when
 * the text is already user copy (a feature's Localized() line). The raw text belongs in the log only; the user
 * sees a plain sentence (B89, 2026-10-23). A dropped call or a connect timeout never gets here: its kind puts it
 * on the call screen (CallMediaErrorShownOnCallScreen).
 */
const char* CallMediaErrorUserKey(const std::string& technical);

/**
 * The failure is already told by the call screen itself (its "Call dropped" / "Couldn't reach them" page): the
 * notice layer says nothing more (#181, one prompt in one place). Read from the error's kind (K014), never its text.
 */
bool CallMediaErrorShownOnCallScreen(const CallMediaError& error);

/**
 * The text is one of the calls feature's own localized notices (CallSessionWorkflow / CallDevice lines), in the
 * current language: shown as is. Any other text that is not a stable code becomes the generic error (#178).
 */
bool CallNoticeIsLocalizedCopy(const std::string& text);

} // namespace pbr
