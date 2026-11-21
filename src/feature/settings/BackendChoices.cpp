#include "feature/settings/BackendChoices.h"

#include "foundation/platform/DeploymentProfile.h"

namespace pbr {

std::vector<const char*> BackendChoicesOffered(const bool staging_compiled, const bool dev_compiled,
                                               const bool tester) {
  std::vector<const char*> offered = {"prod"};
  if (staging_compiled && tester) {
    offered.push_back("staging");
  }
  if (dev_compiled && tester) {
    offered.push_back("dev");
  }
  offered.push_back("custom");
  return offered;
}

bool BackendSwitchNeedsConfirm(const std::string_view current, const std::string_view picked) {
  return ParseBackendEnv(current) != ParseBackendEnv(picked);
}

} // namespace pbr
