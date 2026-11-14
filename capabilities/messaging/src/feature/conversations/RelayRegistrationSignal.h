#pragma once

#include "common/PbrCompat.h"

namespace pbr {

/** What the relay's polls say about this account's registration (B216), for the UI. */
enum class RelayRegistrationSignal {
  /** A poll was answered "Relay user not registered" (coalesced: at most one waiting on the UI thread). */
  NotRegistered,
  /** A poll succeeded after it had not (the relay health changed to Ok). */
  PollOk,
};

} // namespace pbr
