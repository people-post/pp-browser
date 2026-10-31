#pragma once

#include "common/Error.h"
#include "foundation/diagnostics/DiagnosticsUploadResult.h"

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct DiagnosticsUploadDeps {
  std::string data_dir;
  /** Own account id, device id, relay id and every contact's ids; empty entries are skipped. */
  std::vector<std::string> known_identifiers;
  /** The brief gateway Bearer (registered key, else guest key, requesting one when none). Runs on a worker. */
  std::function<Roe<std::string>()> bearer;
  /** Empty = DiagnosticsUploadUrl(). */
  std::string url;
};

/**
 * Bundles this run's and the previous run's diag.log and metrics.log and the pending crash record (read-only)
 * and POSTs them once, on a worker.
 * `on_done` runs on the UI thread. One call at a time (the Settings button is disabled while sending).
 * Records the reference in `{data_dir}/diagnostics/last_upload.json` on success.
 */
void SendDiagnostics(DiagnosticsUploadDeps deps, std::function<void(DiagnosticsUploadResult)> on_done);

} // namespace pbr
