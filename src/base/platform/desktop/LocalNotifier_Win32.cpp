#if defined(_WIN32)

#include "base/platform/desktop/LocalNotifierImpl.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pbr::desktop {

void PostDesktopNotification(const std::string& /*title*/, const std::string& /*body*/) {
  MessageBeep(MB_OK);
}

} // namespace pbr::desktop

#endif
