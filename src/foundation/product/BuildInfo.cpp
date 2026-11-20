#include "foundation/product/BuildInfo.h"

#include "foundation/product/AppVersion.h"

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

std::string FormatBuildIdentity(std::string_view version, std::string_view commit, std::string_view tag) {
  std::string identity(version);
  identity.append(" \xC2\xB7 ").append(FormatBuildLabel(commit, tag));
  return identity;
}

std::string BuildIdentity() {
  return FormatBuildIdentity(AppVersionString(), BuildCommit(), BuildTag());
}

} // namespace pbr
