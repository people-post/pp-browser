#include "foundation/product/BuildInfo.h"

namespace pbr {

std::string FormatBuildLabel(std::string_view commit, std::string_view tag) {
  std::string label;
  if (!tag.empty()) {
    label.append(tag).append(" \xC2\xB7 ");
  }
  label.append(commit);
  return label;
}

std::string BuildLabel() {
  return FormatBuildLabel(BuildCommit(), BuildTag());
}

} // namespace pbr
