#pragma once

#include <string>

namespace pbr {

/**
 * If a pending crash dump exists and the user opted in, POST a compact
 * fingerprint envelope to the org crash ingest URL (relay `/v1/crash-reports`,
 * or client-compat `crash_reports_url` override). Clears the file on success.
 * When consent is off, leaves the file for a later opt-in restart.
 */
void MaybeUploadPendingCrashReport(const std::string& data_dir,
                                   const std::string& profile_data_dir,
                                   bool crash_reports_enabled,
                                   const std::string& relay_base_url);

} // namespace pbr
