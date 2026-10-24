#pragma once

#include "foundation/crypto/IPskSessionStore.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Sessions that came back without this device's numbering (an account backup restored, or a chat reset
 * that kept the sessions): every public-channel pair is marked so that its next send starts the next
 * epoch at seq 1 rather than rewinding in this one (D014, #378). Private-channel sessions are not
 * restored this way and are left alone.
 */
Roe<void> MarkPublicSessionsForRestart(IPskSessionStore& store);

} // namespace pbr
