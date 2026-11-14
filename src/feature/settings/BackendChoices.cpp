#include "feature/settings/BackendChoices.h"

namespace pbr {

std::vector<const char*> BackendChoicesOffered(const bool qa_compiled, const bool tester) {
  std::vector<const char*> offered = {"prod"};
  if (qa_compiled && tester) {
    offered.push_back("qa");
  }
  offered.push_back("custom");
  return offered;
}

} // namespace pbr
