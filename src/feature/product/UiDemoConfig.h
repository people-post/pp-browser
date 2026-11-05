#pragma once

#include <string_view>

namespace pbr {

/**
 * The UI demo app (kUiDemoBuild, DEV/PLAN-ui-demo-data.md) unlocks with a fixed PIN at bootstrap. The config and
 * profile-prefs overrides live in foundation/data/UiDemoConfig.h, where every Config producer calls them.
 */

/** The demo app's fixed unlock PIN. Only protects the demo app's local vault; never used by the real app. */
inline constexpr const char* kUiDemoPin = "123456";

/** The PIN bootstrap unlocks with: the fixed demo PIN when `ui_demo`, else `given` unchanged (a view, no copy). */
std::string_view ResolveBootstrapPin(bool ui_demo, std::string_view given);

} // namespace pbr
