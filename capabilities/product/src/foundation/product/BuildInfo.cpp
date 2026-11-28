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

std::string TesterBuildNumber(const bool tester, const std::string_view tag, const std::string_view version) {
  if (!tester) {
    return {};
  }
  constexpr std::string_view kInternal = "internal/";
  if (tag.substr(0, kInternal.size()) == kInternal) {
    const std::string_view number = tag.substr(kInternal.size());
    if (!number.empty() && number.find_first_not_of("0123456789") == std::string_view::npos) {
      return std::string(number);
    }
  }
  return std::string(version);
}

} // namespace pbr
