#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

enum class DiagnosticsUploadKind {
  Sent,
  SelfCheckRefused,
  NoNetwork,
  QuotaKey,
  QuotaIp,
  QuotaGlobal,
  InProgress,
  Unavailable,
  TooLarge,
  Failed,
};

struct DiagnosticsUploadResult {
  DiagnosticsUploadKind kind = DiagnosticsUploadKind::Failed;
  std::string upload_id;  // Sent
  int lines = 0;          // Sent
  std::string detail;     // SelfCheckRefused: detector name; Failed: what went wrong (no identifiers)
};

/** An HTTP answer (status + body) as a result. 2xx without an `upload_id` is a Failed. */
DiagnosticsUploadResult MapDiagnosticsResponse(long status, const std::string& body);

/** True when the attempt ended without an answer, so a retry keeps the same client_token (server dedupes). */
bool DiagnosticsKindKeepsToken(DiagnosticsUploadKind kind);

/** The in-memory client_token: reused only after an attempt that ended without a result. */
class DiagnosticsClientToken {
public:
  /** The token for the next attempt (a new UUID unless the previous attempt asked to keep it). */
  const std::string& Acquire();
  /** Records how the attempt ended; a conclusive result drops the token. */
  void Finish(DiagnosticsUploadKind kind);

private:
  std::string token_;
};

struct LastUpload {
  std::string upload_id;
  int64_t at_ms = 0;
};

/** `{data_dir}/diagnostics/last_upload.json` */
std::string LastUploadPath(const std::string& data_dir);
/** Owner-only file; false when it could not be written. */
bool WriteLastUpload(const std::string& data_dir, const LastUpload& last);
std::optional<LastUpload> ReadLastUpload(const std::string& data_dir);

} // namespace pbr
