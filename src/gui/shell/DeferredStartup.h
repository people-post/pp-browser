#pragma once

namespace pbr {

class ClientCompatController;
class ProfileUnlockGate;
struct ShellNavigationPorts;

/** True when the resolved UI language needs CJK fallback glyphs for chrome strings. */
bool UiLanguageNeedsCjkFonts();

/**
 * Registers the bundled Simplified Chinese faces as one fallback family in three weights (400 / 500 /
 * 700), so Chinese in medium or bold text gets a heavier face; deferred startup loads them in the
 * collection's SC slot. False when the regular face could not be loaded.
 */
bool LoadChineseFonts();

/**
 * After the first successful Present: load deferred font faces and start vault unlock.
 * Safe to call once; subsequent calls no-op.
 */
void OnFirstPresentDeferredStartup(ClientCompatController& client_compat, ProfileUnlockGate& unlock_gate,
                                   const ShellNavigationPorts& shell);

} // namespace pbr
