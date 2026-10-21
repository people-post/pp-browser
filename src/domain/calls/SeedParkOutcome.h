#pragma once

#include <cstdint>

namespace pbr {

/**
 * Reads the result of a bootstrap seed park for the call screen: did this connect fail because no
 * seed was reachable (a VPN or firewall dropping UDP)? Pure; the bridge owns the clock and epoch.
 */
struct SeedParkOutcome {
  /** A park that settles this much earlier than its deadline did not time out. */
  static constexpr int kTimeoutSlackMs = 2000;

  /**
   * `parked == false` also covers "no seeds configured", "mesh not up" and "abandoned on stop",
   * which all settle at once; only a park that ran to its deadline means the seeds were unreachable.
   * The epoch moves whenever the connect-failed state is cleared (new call, retry, stop), so a park
   * from an earlier attempt cannot speak for the current one.
   */
  static bool SeedUnreachable(bool parked, int64_t elapsed_ms, int timeout_ms, uint64_t epoch_at_start,
                              uint64_t epoch_now) {
    return !parked && epoch_at_start == epoch_now && elapsed_ms + kTimeoutSlackMs >= timeout_ms;
  }
};

} // namespace pbr
