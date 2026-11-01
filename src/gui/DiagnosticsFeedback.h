#pragma once

#include "foundation/diagnostics/DiagnosticsUploadResult.h"

#include <string>

namespace pbr {

/** Locale key of the message for a send result. `have_last_id`: a reference is on file (QuotaKey names it). */
std::string DiagnosticsResultKey(const DiagnosticsUploadResult& result, bool have_last_id);

/** The user-facing sentence for a send result (never suggests checking the network or permissions). */
std::string DiagnosticsResultText(const DiagnosticsUploadResult& result, const std::string& last_id);

} // namespace pbr
