#ifndef PP_BROWSER_BUILD_INFO_H
#define PP_BROWSER_BUILD_INFO_H

#include <string>
#include <string_view>

namespace pbr {

/** The commit this binary was built from: 8 hex (the one the build was given, PP_BUILD_COMMIT, else git's HEAD plus
 *  "-dirty" when tracked files had uncommitted changes), or "unknown" without either. Generated on every build
 *  (GenBuildInfo.cmake). */
const char* BuildCommit();

/** The build's tag (e.g. "internal/141", "v1.0.0-rc.3") when the build passed one (PP_BUILD_TAG) or a clean HEAD
 *  carries a build tag, else "". */
const char* BuildTag();

/** What About shows after "Build": "internal/141 · a1b2c3d4", or just the commit without a tag. */
std::string FormatBuildLabel(std::string_view commit, std::string_view tag);

/** FormatBuildLabel of this build. */
std::string BuildLabel();

/** What diagnostics name the build by: "1.0.0 · v1.0.0-rc.3 · a1b2c3d4", or "1.0.0 · a1b2c3d4" without a tag. */
std::string FormatBuildIdentity(std::string_view version, std::string_view commit, std::string_view tag);

/** FormatBuildIdentity of this build (AppVersionString, BuildCommit, BuildTag). */
std::string BuildIdentity();

} // namespace pbr

#endif // PP_BROWSER_BUILD_INFO_H
