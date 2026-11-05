#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * A screen with a secret on it is up (the PIN gate; Me → Account & security, where the recovery phrase shows), or
 * not. The OS's copies of the screen leave it out: on Android no screenshot, screen recording, cast or Recents
 * preview of it (FLAG_SECURE); on iOS the app switcher shows a plain cover instead; on Windows and macOS the window is
 * left out of screen sharing and screenshots (where the capturing app honours it). Linux: nothing.
 *
 * UI thread; ShellHost calls them on a change.
 */
namespace SecureScreen {

void SetSecretOnScreen(bool on);
/**
 * A secret can be read on screen now (the recovery phrase shown) — not only typed. iOS cannot keep a recording or a
 * mirror from copying the screen, so while one runs a cover hides the app ("Hidden while your screen is recorded or
 * shared"). The PIN gate is not one: a cover there would keep the app locked while mirrored to a TV.
 */
void SetSecretLegible(bool on);

} // namespace SecureScreen

} // namespace pbr
