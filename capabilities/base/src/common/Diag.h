#pragma once

#include "common/Logger.h"
#include "common/ShareableLine.h"

#include "common/PbrCompat.h"

namespace pbr {

/**
 * The "Diag" channel: troubleshooting events, in `logs/diag.log` (only these lines). It goes with the developer log
 * (`pp-browser.log`) only when the user sends their run logs (Settings → Advanced → Send run logs, test phase).
 * What may go in a line, and how to add an event: docs/contracts/DIAGNOSTICS.md.
 */
inline logging::Logger& DiagLog() {
  static logging::Logger log = [] {
    logging::Logger l = logging::getLogger("Diag");
    l.setLevel(logging::Level::INFO); // always on, whatever the developer log level
    return l;
  }();
  return log;
}

/** `DiagLine("relay.http_failed").Add("action", "send").Add("status", status).Emit();` */
class DiagLine : public ShareableLine {
public:
  explicit DiagLine(ShareWord event) : ShareableLine(DiagLog(), event) {}
};

} // namespace pbr
