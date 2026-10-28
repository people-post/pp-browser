#include "gui/DiagnosticsFeedback.h"

#include "foundation/i18n/LocalizationService.h"

namespace pbr {

std::string DiagnosticsResultKey(const DiagnosticsUploadResult& result, const bool have_last_id) {
  switch (result.kind) {
  case DiagnosticsUploadKind::Sent:
    return "settings.security.diagnostics.sent";
  case DiagnosticsUploadKind::SelfCheckRefused:
    return "settings.security.diagnostics.refused";
  case DiagnosticsUploadKind::NoNetwork:
    return "settings.security.diagnostics.no_network";
  case DiagnosticsUploadKind::QuotaKey:
    return have_last_id ? "settings.security.diagnostics.quota_key" : "settings.security.diagnostics.quota_key_no_id";
  case DiagnosticsUploadKind::QuotaIp:
  case DiagnosticsUploadKind::QuotaGlobal:
    return "settings.security.diagnostics.quota_limit";
  case DiagnosticsUploadKind::InProgress:
    return "settings.security.diagnostics.in_progress";
  case DiagnosticsUploadKind::Unavailable:
    return "settings.security.diagnostics.unavailable";
  case DiagnosticsUploadKind::TooLarge:
    return "settings.security.diagnostics.too_large";
  case DiagnosticsUploadKind::Failed:
    break;
  }
  return "settings.security.diagnostics.failed";
}

std::string DiagnosticsResultText(const DiagnosticsUploadResult& result, const std::string& last_id) {
  const std::string key = DiagnosticsResultKey(result, !last_id.empty());
  switch (result.kind) {
  case DiagnosticsUploadKind::Sent:
    return Tr(key, {{"id", result.upload_id}});
  case DiagnosticsUploadKind::QuotaKey:
    return last_id.empty() ? Tr(key) : Tr(key, {{"id", last_id}});
  case DiagnosticsUploadKind::Failed:
    return Tr(key, {{"detail", result.detail}});
  default:
    return Tr(key);
  }
}

} // namespace pbr
