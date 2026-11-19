#ifndef PP_BROWSER_BUILD_INFO_H
#define PP_BROWSER_BUILD_INFO_H

#include <string>
#include <string_view>

namespace pbr {

/** The commit this binary was built from: 8 hex, plus "-dirty" when tracked files had uncommitted changes, or
 *  "unknown" when git was not available. Generated on every build (GenBuildInfo.cmake). */
const char* BuildCommit();

/** The build's tag (e.g. "internal/141") when the build passed one (PP_BUILD_TAG), else "". */
const char* BuildTag();

/** What About shows after "Build": "internal/141 · a1b2c3d4", or just the commit without a tag. */
std::string FormatBuildLabel(std::string_view commit, std::string_view tag);

/** FormatBuildLabel of this build. */
std::string BuildLabel();

} // namespace pbr

#endif // PP_BROWSER_BUILD_INFO_H
