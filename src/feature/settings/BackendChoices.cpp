#include "feature/settings/BackendChoices.h"

#include "foundation/platform/DeploymentProfile.h"

namespace pbr {

std::vector<const char*> BackendChoicesOffered(const bool prod_available, const bool staging_compiled,
                                               const bool dev_compiled, const bool tester_options) {
  std::vector<const char*> offered;
  if (prod_available) {
    offered.push_back("prod");
  }
  // Behind the tester options only where they keep a store user on prod; a tester build has no prod to keep.
  const bool testers_shown = tester_options || !prod_available;
  if (staging_compiled && testers_shown) {
    offered.push_back("staging");
  }
  if (dev_compiled && testers_shown) {
    offered.push_back("dev");
  }
  offered.push_back("custom");
  return offered;
}

bool BackendSwitchNeedsConfirm(const std::string_view current, const std::string_view picked) {
  return ParseBackendEnv(current) != ParseBackendEnv(picked);
}

} // namespace pbr
