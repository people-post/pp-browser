#include "foundation/diagnostics/CrashProbe.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

namespace pbr {

void CrashProbe::Start(const std::string&) {
  logging::getLogger("CrashProbe").warning << "probe: no system diagnostics collector on this platform";
}

} // namespace pbr
