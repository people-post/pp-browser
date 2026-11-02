#include "app/DiagnosticsUpload.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "common/Utilities.h"
#include "foundation/diagnostics/CrashDump.h"
#include "foundation/error/AppError.h"
#include "foundation/diagnostics/CrashReportEnvelope.h"
#include "foundation/diagnostics/DiagnosticsBundle.h"
#include "foundation/diagnostics/LogFile.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/os/OsTime.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/product/AppVersion.h"
#include "foundation/product/ProductBranding.h"
#include "lib/net/HttpClient.h"

#include <cstdio>
#include <ctime>
#include <utility>

namespace pbr {

namespace {

constexpr long kUploadTimeoutSeconds = 60;

// The client_token lives for the process; touched on the UI thread only (Acquire before the worker, Finish in the reply).
DiagnosticsClientToken& ClientToken() {
  static DiagnosticsClientToken token;
  return token;
}

/** "YYYY-MM-DD HH:MM:SS.mmm" (UTC) for the crash lines, the same shape as a log line's stamp. */
std::string UtcStamp(const int64_t unix_ms) {
  std::tm utc{};
  (void)os::UtcTime(static_cast<std::time_t>(unix_ms / 1000), &utc);
  char buf[96]; // room for any int in each field (GCC -Wformat-truncation)
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(unix_ms % 1000));
  return buf;
}

DiagnosticsUploadResult Failed(DiagnosticsUploadKind kind, std::string detail = {}) {
  DiagnosticsUploadResult result;
  result.kind = kind;
  result.detail = std::move(detail);
  return result;
}

DiagnosticsUploadResult Upload(const DiagnosticsUploadDeps& deps, const std::string& client_token) {
  auto log = logging::getLogger("Diagnostics");

  DiagnosticsBundleInput input;
  // The channel files only: the developer log (pp-browser.log) never leaves the device.
  const std::string log_path = LogFile::DefaultPath(deps.data_dir);
  const std::string diag = LogFile::DiagPath(log_path);
  const std::string metrics = LogFile::MetricsPath(log_path);
  // Four entries at most with the crash record (kDiagnosticsMaxFiles): the previous run's metrics stay home.
  input.files = {diag, metrics, LogFile::RotatedPath(diag, 1)};
  const int64_t now_ms = util::NowUnixMs();
  if (CrashDump::HasPending(deps.data_dir)) {
    const std::string dump = CrashDump::ReadPending(deps.data_dir);  // read-only: the crash channel clears it
    if (!dump.empty()) {
      input.crash_lines = CrashEnvelopeLines(BuildCrashReportEnvelope(dump), UtcStamp(now_ms));
    }
  }
  for (const std::string& id : deps.known_identifiers) {
    if (!id.empty()) {
      input.known_identifiers.push_back(id);
    }
  }
  input.client_token = client_token;
  input.product = kProductName;
  input.version = AppVersionString();
  input.os = CrashDump::OsLabel();
  input.captured_at_ms = now_ms;

  auto bundle = BuildDiagnosticsBundle(input);
  if (!bundle) {
    static constexpr char kPrefix[] = "self-check: ";
    const std::string& message = bundle.error().message;
    if (message.rfind(kPrefix, 0) == 0) {
      const std::string detector = message.substr(sizeof(kPrefix) - 1);
      log.warning << "Diagnostics withheld by self-check detector=" << detector;  // the detector, never the hit
      return Failed(DiagnosticsUploadKind::SelfCheckRefused, detector);
    }
    log.warning << "Diagnostics bundle failed: " << message;
    return Failed(DiagnosticsUploadKind::Failed, message);
  }

  Roe<std::string> key = deps.bearer ? deps.bearer() : Roe<std::string>{Error("no key")};
  if (!key || key.value().empty()) {
    log.info << "Diagnostics not sent: no gateway key";
    return Failed(DiagnosticsUploadKind::Unavailable);
  }

  const std::string url = deps.url.empty() ? DiagnosticsUploadUrl() : deps.url;
  if (url.empty()) {
    log.info << "Diagnostics not sent: no upload URL";
    return Failed(DiagnosticsUploadKind::Unavailable);
  }
  HttpTimeout timeout;
  timeout.total_s = kUploadTimeoutSeconds;
  log.info << "Uploading diagnostics lines=" << bundle->lines << " bytes=" << bundle->bytes;
  auto response = HttpClient::Post(url, bundle->json,
                                   {{"Content-Type", "application/json"},
                                    {"X-PP-Diagnostics", "1"},
                                    {"Authorization", "Bearer " + key.value()}},
                                   kMaxHttpClientBodyBytes, timeout);
  if (!response) {
    log.warning << "Diagnostics upload failed: " << response.error().message;
    if (response.error().category == static_cast<int32_t>(ErrorCategory::Network)) {
      return Failed(DiagnosticsUploadKind::NoNetwork);
    }
    return Failed(DiagnosticsUploadKind::Failed, response.error().message);
  }
  DiagnosticsUploadResult result = MapDiagnosticsResponse(response->status_code, response->body);
  log.info << "Diagnostics upload answered HTTP " << response->status_code;
  if (result.kind == DiagnosticsUploadKind::Sent) {
    if (!WriteLastUpload(deps.data_dir, LastUpload{result.upload_id, now_ms})) {
      log.warning << "Could not record the diagnostics reference";
    }
  }
  return result;
}

} // namespace

void SendDiagnostics(DiagnosticsUploadDeps deps, std::function<void(DiagnosticsUploadResult)> on_done) {
  const std::string client_token = ClientToken().Acquire();
  AppRuntime::PostWorkerAndReplyOnUI<DiagnosticsUploadResult>(
      WorkerLane::Normal,
      [deps = std::move(deps), client_token]() { return Upload(deps, client_token); },
      [on_done = std::move(on_done)](DiagnosticsUploadResult result) {
        ClientToken().Finish(result.kind);
        if (on_done) {
          on_done(std::move(result));
        }
      });
}

} // namespace pbr
