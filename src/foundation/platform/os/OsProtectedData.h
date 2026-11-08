#pragma once

#include <cstdint>
#include <functional>

#include "foundation/crypto/GraceUnlock.h"

namespace pbr::os {

/**
 * Whether the OS lets the app read its protected data right now (iOS: UIApplication.protectedDataAvailable, false
 * while the device is locked and the app was started from a push). Always true where there is no such state.
 */
bool ProtectedDataAvailable();

using ProtectedDataHandlerId = uint64_t;
/**
 * Runs `fn` (on an unspecified thread) each time protected data becomes available again (iOS
 * UIApplicationProtectedDataDidBecomeAvailable; Android ACTION_USER_PRESENT). Never runs where there is no such
 * state. Register before the first read that may be Unavailable, then read once more: the two can race.
 */
ProtectedDataHandlerId OnProtectedDataAvailable(std::function<void()> fn);
void RemoveProtectedDataHandler(ProtectedDataHandlerId id);

/** The device's boot session and monotonic clock for the PIN grace window (GraceClock). */
GraceClock GraceClockNow();

} // namespace pbr::os
