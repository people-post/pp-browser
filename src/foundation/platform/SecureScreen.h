#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/**
 * A screen with a secret on it is up (the PIN gate; Me → Account & security, where the recovery phrase shows), or
 * not. The OS's copies of the screen leave it out: on Android no screenshot, screen recording, cast or Recents
 * preview of it (FLAG_SECURE); on iOS the app switcher shows a plain cover instead. Elsewhere nothing.
 *
 * UI thread; ShellHost calls it on a change.
 */
namespace SecureScreen {

void SetSecretOnScreen(bool on);

} // namespace SecureScreen

} // namespace pbr
