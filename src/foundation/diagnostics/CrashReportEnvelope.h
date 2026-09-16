#pragma once

#include <string>
#include <vector>

namespace pbr {

/** Compact, privacy-safe payload for POST /v1/crash-reports (server fingerprints / clusters). */
struct CrashReportEnvelope {
  static constexpr int kSchemaVersion = 1;

  int schema_version = kSchemaVersion;
  std::string product;
  std::string version;
  std::string os;
  std::string reason;
  std::string fingerprint;
  std::vector<std::string> frames;
  std::vector<std::string> breadcrumb_tags;
  /** Short sample of raw dump (truncated); not chat content. */
  std::string sample;
};

/** Build envelope + fingerprint from a pending crash dump text body. */
CrashReportEnvelope BuildCrashReportEnvelope(const std::string& dump_text);

/** JSON body for HTTP upload. */
std::string CrashReportEnvelopeToJson(const CrashReportEnvelope& envelope);

/**
 * Resolve ingest URL: non-empty override wins; else `{relay_base_url}/v1/crash-reports`.
 * Empty when neither is usable.
 */
std::string ResolveCrashReportUploadUrl(const std::string& relay_base_url,
                                        const std::string& crash_reports_url_override);

} // namespace pbr
