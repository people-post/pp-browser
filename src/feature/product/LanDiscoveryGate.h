#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace pbr {

/**
 * Holds the start of LAN (mDNS) discovery until the app says it may run. On iOS the first multicast use raises the
 * system's "local network" alert, which must not cover the account-setup page: the app lets discovery start once setup
 * is finished. Only this one operation waits; the rest of the mesh comes up as always.
 *
 * Open from the start (`allowed` true): a request starts at once, as before. Closed: the request and the latest
 * advertisement are kept, and run once when `SetAllowed(true)` comes — once, even if it comes again. Any thread.
 */
class LanDiscoveryGate {
public:
  struct Ops {
    std::function<void(const std::string& peer_id, int amp_udp_port)> advertise;
    std::function<void()> stop;
  };

  explicit LanDiscoveryGate(Ops ops) : ops_(std::move(ops)) {}

  /** The app's word: setup is finished (true) or not (false). Starts a held request when it opens. */
  void SetAllowed(const bool allowed) {
    std::lock_guard<std::mutex> lock(mu_);
    allowed_ = allowed;
    StartIfDue();
  }

  /** Start discovery with `start` (run now when allowed, else held). Replaces an earlier request. */
  void Request(std::function<void()> start) {
    std::lock_guard<std::mutex> lock(mu_);
    CancelLocked();
    start_ = std::move(start);
    StartIfDue();
  }

  /** The latest advertisement; sent now when discovery runs, else when it starts. */
  void Advertise(const std::string& peer_id, const int amp_udp_port) {
    std::lock_guard<std::mutex> lock(mu_);
    advertisement_ = std::make_pair(peer_id, amp_udp_port);
    if (started_ && ops_.advertise) {
      ops_.advertise(peer_id, amp_udp_port);
    }
  }

  /** Stops what runs and forgets what was held. */
  void Cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    CancelLocked();
  }

  bool Started() const {
    std::lock_guard<std::mutex> lock(mu_);
    return started_;
  }

private:
  void StartIfDue() {
    if (!allowed_ || started_ || !start_) {
      return;
    }
    started_ = true;
    start_();
    if (advertisement_ && ops_.advertise) {
      ops_.advertise(advertisement_->first, advertisement_->second);
    }
  }
  void CancelLocked() {
    if (started_ && ops_.stop) {
      ops_.stop();
    }
    started_ = false;
    start_ = nullptr;
    advertisement_.reset();
  }

  Ops ops_;
  mutable std::mutex mu_;
  bool allowed_ = true;
  bool started_ = false;
  std::function<void()> start_;
  std::optional<std::pair<std::string, int>> advertisement_;
};

} // namespace pbr
