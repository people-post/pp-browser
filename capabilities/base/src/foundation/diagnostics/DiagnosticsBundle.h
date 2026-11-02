#pragma once

#include "common/PbrCompat.h"
#include "foundation/diagnostics/CrashReportEnvelope.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * The only channels whose lines leave the device (docs/contracts/DIAGNOSTICS.md): each writes shareable
 * lines (common/ShareableLine.h) to a file of its own, and the crash record is rebuilt as "Crash" lines.
 * The developer log (pp-browser.log) is never read.
 */
inline constexpr std::string_view kDiagnosticsChannels[] = {"Diag", "Metrics", "Crash"};

inline constexpr std::size_t kDiagnosticsMaxJsonBytes = 2u * 1024u * 1024u;
/** File entries in one upload, the crash record's included: the gateway and app-support accept at most 4. */
inline constexpr std::size_t kDiagnosticsMaxFiles = 4;
inline constexpr int kDiagnosticsMaxLines = 30000;
inline constexpr std::size_t kDiagnosticsMaxLineBytes = 2048;

struct DiagnosticsBundleInput {
  /**
   * Channel files, most wanted first (dropped last when over the cap): diag.log, metrics.log, diag.1.log. At most
   * kDiagnosticsMaxFiles - 1: the crash record is an entry too.
   */
  std::vector<std::filesystem::path> files;
  std::vector<std::string> crash_lines;          // already formatted "[ts] [INFO] [Crash] ..." lines; kept longest
  std::vector<std::string> known_identifiers;    // own account id, contact peer ids, device id, relay id
  std::string client_token, product, version, os;
  int64_t captured_at_ms = 0;
};

struct DiagnosticsBundle {
  std::string json;
  int lines = 0;
  std::size_t bytes = 0;
};

/**
 * Filter (prefix, level >= INFO, kDiagnosticsChannels) -> cap -> self-check -> JSON. Nothing is rewritten:
 * the files hold shareable lines only. On a self-check hit returns the error "self-check: <detector>" and
 * builds nothing.
 */
Roe<DiagnosticsBundle> BuildDiagnosticsBundle(const DiagnosticsBundleInput& input);

/**
 * Crash envelope as "[ts] [INFO] [Crash] key=value" lines: reason, frames, breadcrumb tags. Not the raw
 * sample of the dump.
 */
std::vector<std::string> CrashEnvelopeLines(const CrashReportEnvelope& envelope, std::string_view timestamp);

} // namespace pbr
