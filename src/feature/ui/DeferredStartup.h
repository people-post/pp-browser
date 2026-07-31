#pragma once

namespace pbr {

class ClientCompatController;

/** True when the resolved UI language needs CJK fallback glyphs for chrome strings. */
bool UiLanguageNeedsCjkFonts();

/**
 * After the first successful Present: load deferred font faces and start vault unlock.
 * Safe to call once; subsequent calls no-op.
 */
void OnFirstPresentDeferredStartup(ClientCompatController& client_compat);

} // namespace pbr
