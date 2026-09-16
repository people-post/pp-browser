#pragma once

#include <string>

namespace pbr {

/**
 * If a pending crash dump exists and the user opted in, POST it to
 * PP_BROWSER_CRASH_REPORT_URL (no-op when unset). Clears the file on success.
 * When consent is off, leaves the file in place for a later opt-in restart.
 */
void MaybeUploadPendingCrashReport(const std::string& data_dir, bool crash_reports_enabled);

} // namespace pbr
