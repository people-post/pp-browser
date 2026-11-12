#pragma once

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

/**
 * Chat picture previews arrive one by one (two are made at a time); the rows showing them are redrawn at most once
 * every kInterval with all that came meanwhile (B203). Add says when a redraw must be scheduled (nullopt: one is
 * already); Take, at that redraw, hands over what came.
 */
class PreviewRedrawBatcher {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kInterval{250};

  /** `key` is ready. The delay of the redraw to schedule now (0: at once), or nullopt when one is scheduled already. */
  std::optional<std::chrono::milliseconds> Add(std::string key, const Clock::time_point now) {
    keys_.insert(std::move(key));
    if (scheduled_) {
      return std::nullopt;
    }
    scheduled_ = true;
    if (!last_ || now >= *last_ + kInterval) {
      return std::chrono::milliseconds(0);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(*last_ + kInterval - now);
  }
  /** The scheduled redraw runs: what is ready since the last one. */
  std::vector<std::string> Take(const Clock::time_point now) {
    scheduled_ = false;
    last_ = now;
    std::vector<std::string> out(keys_.begin(), keys_.end());
    keys_.clear();
    return out;
  }
  /** Another chat (or none): what was waiting is not for its rows. A redraw already scheduled finds nothing. */
  void Clear() { keys_.clear(); }

private:
  std::set<std::string> keys_;
  bool scheduled_ = false;
  std::optional<Clock::time_point> last_;
};

} // namespace pbr
