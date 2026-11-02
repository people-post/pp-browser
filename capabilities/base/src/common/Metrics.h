#pragma once

#include "common/Logger.h"
#include "common/ShareableLine.h"

#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Operational metrics (Kenneth 2026-09-29): one line per event on its own "Metrics" channel —
 * `event=<name> k=v k=v…`, stable names and keys. Timings, rates and device load. The line is a
 * ShareableLine: it cannot carry content, names, raw ids or addresses, and `metrics.log` is part of
 * the diagnostics a user can send (docs/contracts/DIAGNOSTICS.md).
 */
inline logging::Logger& MetricsLog() {
  static logging::Logger log = [] {
    logging::Logger l = logging::getLogger("Metrics");
    l.setLevel(logging::Level::INFO);  // always on, whatever the developer log level
    return l;
  }();
  return log;
}

class MetricsLine : public ShareableLine {
public:
  explicit MetricsLine(ShareWord event) : ShareableLine(MetricsLog(), event) {}
};

/** The call id as metrics name it: its first 8 hex, without the "call:" prefix. */
inline std::string MetricsCallId(std::string_view call_id) { return ShortCallId(call_id); }

} // namespace pbr
