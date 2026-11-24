#pragma once

namespace pbr {

/**
 * Fills media's host ports (lib/media/MediaHost.h, projects/capability-repos CR023 step 5) with this app's platform
 * side: on Android the activity's AudioManager work and its device vitals (MediaHost_Android.cpp, which also holds the
 * activity's native callbacks into media); elsewhere media asks nothing (MediaHost_Default.cpp). Once, at startup,
 * before any call.
 */
void InstallMediaHost();

} // namespace pbr
