#pragma once

#include "common/Logger.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Operational metrics (Kenneth 2026-09-29): one line per event on its own "Metrics" channel —
 * `event=<name> k=v k=v…`, stable names and keys, for a future collector that reads this channel
 * only. Timings, rates and device load; NEVER content, account ids, peer ids, names or addresses
 * (the developer log carries those; monitoring must not). A call is named by the first 8 hex of
 * its random call id, only to join the two sides' lines.
 */
inline logging::Logger& MetricsLog() {
  static logging::Logger log = [] {
    logging::Logger l = logging::getLogger("Metrics");
    l.setLevel(logging::Level::INFO);  // always on, whatever the developer log level
    return l;
  }();
  return log;
}

class MetricsLine {
public:
  explicit MetricsLine(std::string_view event) { out_ << "event=" << event; }

  MetricsLine& Add(std::string_view key, std::string_view value) {
    out_ << ' ' << key << '=' << (value.empty() ? std::string_view("-") : value);
    return *this;
  }
  MetricsLine& Add(std::string_view key, const char* value) { return Add(key, std::string_view(value ? value : "")); }
  MetricsLine& Add(std::string_view key, const std::string& value) { return Add(key, std::string_view(value)); }
  MetricsLine& Add(std::string_view key, int64_t value) {
    out_ << ' ' << key << '=' << value;
    return *this;
  }
  MetricsLine& Add(std::string_view key, int value) { return Add(key, static_cast<int64_t>(value)); }
  MetricsLine& Add(std::string_view key, uint64_t value) {
    out_ << ' ' << key << '=' << value;
    return *this;
  }
  MetricsLine& Add(std::string_view key, bool value) { return Add(key, static_cast<int64_t>(value ? 1 : 0)); }
  /** One decimal place (rates, seconds). */
  MetricsLine& Add(std::string_view key, double value) {
    out_ << ' ' << key << '=' << std::fixed << std::setprecision(1) << value;
    return *this;
  }

  std::string str() const { return out_.str(); }
  void Emit() const { MetricsLog().info << out_.str(); }

private:
  std::ostringstream out_;
};

/** The call id as metrics name it: its first 8 hex, without the "call:" prefix. */
inline std::string MetricsCallId(std::string_view call_id) {
  if (call_id.substr(0, 5) == "call:") {
    call_id.remove_prefix(5);
  }
  return std::string(call_id.substr(0, 8));
}

} // namespace pbr
