#pragma once

#include "foundation/crypto/PinDefaults.h"
#include "foundation/i18n/LocalizationService.h"

#include <string>

namespace pbr {

/**
 * What the "?" after the default-PIN line says. The digits come from kDefaultProfilePin at run time, never from a
 * locale file or a view, so the PIN has one place to change.
 */
inline std::string DefaultPinNoteText() {
  return Tr("settings.security.default_hint.note", {{"pin", kDefaultProfilePin}});
}

} // namespace pbr
