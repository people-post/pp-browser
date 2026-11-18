#include "foundation/platform/MobileOrientation.h"

#include "foundation/platform/Platform.h"

namespace pbr {

const char* MobileOrientationsHint() {
  return Platform::IsMobile() ? PhoneOrientationsHint() : nullptr; // only Android is mobile here
}

} // namespace pbr
