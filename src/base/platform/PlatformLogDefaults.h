#pragma once

#include "common/Logger.h"

namespace pbr {

logging::Level DefaultRootLogLevel(bool debug_mode);

/** Process-wide emit floor for Install at startup (see logging::setEmitFloor).
 *  Android release: WARNING so INFO call sites survive `adb logcat -s pp-browser:W`.
 *  Elsewhere / debug: DEBUG (no boost). */
logging::Level DefaultEmitFloor(bool debug_mode);

} // namespace pbr
