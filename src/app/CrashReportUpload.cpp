#include "app/CrashReportUpload.h"

#include "domain/net/ClientCompat.h"
#include "domain/net/HttpClient.h"
#include "foundation/diagnostics/CrashDump.h"
#include "foundation/diagnostics/CrashReportEnvelope.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <chrono>

namespace pbr {

namespace {

std::string CachedCrashReportsUrlOverride(const std::string& profile_data_dir) {
  if (profile_data_dir.empty()) {
    return {};
  }
  auto cache = LoadClientCompatCache(profile_data_dir);
  if (!cache) {
    return {};
  }
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (!ClientCompatCacheFresh(*cache, now)) {
    return {};
  }
  return cache->document.crash_reports_url;
}

} // namespace

void MaybeUploadPendingCrashReport(const std::string& data_dir,
                                   const std::string& profile_data_dir,
                                   bool crash_reports_enabled,
                                   const std::string& relay_base_url) {
  if (data_dir.empty() || !CrashDump::HasPending(data_dir)) {
    return;
  }

  auto log = logging::getLogger("CrashReport");
  const std::string path = CrashDump::PendingPath(data_dir);

  if (!crash_reports_enabled) {
    log.info << "Pending crash dump kept (reports disabled): " << path;
    return;
  }

  const std::string url =
      ResolveCrashReportUploadUrl(relay_base_url, CachedCrashReportsUrlOverride(profile_data_dir));
  if (url.empty()) {
    log.info << "Pending crash dump present but relay base URL empty; keeping " << path;
    return;
  }

  const std::string dump = CrashDump::ReadPending(data_dir);
  if (dump.empty()) {
    CrashDump::ClearPending(data_dir);
    return;
  }

  const CrashReportEnvelope envelope = BuildCrashReportEnvelope(dump);
  const std::string body = CrashReportEnvelopeToJson(envelope);
  log.info << "Uploading crash envelope fingerprint=" << envelope.fingerprint << " bytes=" << body.size()
           << " url=" << url;

  auto response = HttpClient::Post(url, body, {{"Content-Type", "application/json"},
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
