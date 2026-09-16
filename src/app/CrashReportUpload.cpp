#include "app/CrashReportUpload.h"

#include "domain/net/HttpClient.h"
#include "foundation/diagnostics/CrashDump.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <cstdlib>

namespace pbr {

void MaybeUploadPendingCrashReport(const std::string& data_dir, bool crash_reports_enabled) {
  if (data_dir.empty() || !CrashDump::HasPending(data_dir)) {
    return;
  }

  auto log = logging::getLogger("CrashReport");
  const std::string path = CrashDump::PendingPath(data_dir);

  if (!crash_reports_enabled) {
    log.info << "Pending crash dump kept (reports disabled): " << path;
    return;
  }

  const char* url = std::getenv("PP_BROWSER_CRASH_REPORT_URL");
  if (url == nullptr || url[0] == '\0') {
    log.info << "Pending crash dump present but PP_BROWSER_CRASH_REPORT_URL unset; keeping "
             << path;
    return;
  }

  const std::string body = CrashDump::ReadPending(data_dir);
  if (body.empty()) {
    CrashDump::ClearPending(data_dir);
    return;
  }

  log.info << "Uploading crash dump (" << body.size() << " bytes) to configured URL";
  auto response = HttpClient::Post(url, body, {{"Content-Type", "text/plain; charset=utf-8"},
                                               {"X-PP-Crash-Report", "1"}});
  if (!response) {
    log.warning << "Crash dump upload failed: " << response.error().message;
    return;
  }
  if (response->status_code < 200 || response->status_code >= 300) {
    log.warning << "Crash dump upload HTTP " << response->status_code;
    return;
  }

  CrashDump::ClearPending(data_dir);
  log.info << "Crash dump uploaded and cleared";
}

} // namespace pbr
