#pragma once

#include "foundation/data/Config.h"
#include "foundation/data/UserPreferences.h"

namespace pbr {

/**
 * The UI demo app (kUiDemoBuild, DEV/PLAN-ui-demo-data.md) has no backend and no mesh. Config::DefaultAppConfig,
 * Config::LoadFromFile and SessionStore's profile-prefs loads apply these whenever their `ui_demo` is true
 * (default kUiDemoBuild), so every producer of a Config / ProfilePreferences, including the reloads after a reset,
 * yields the demo values. Always compiled, so the normal build's tests cover them.
 */

/** Clears every backend address (relay / directory / registration / LLM / MCP), turns the mesh off, pins `prod`. */
void ApplyUiDemoConfig(AppConfig& config);

/** Marks onboarding done: the demo app opens on the home screen with no name / registration step. */
void ApplyUiDemoProfilePrefs(ProfilePreferences& prefs);

} // namespace pbr
