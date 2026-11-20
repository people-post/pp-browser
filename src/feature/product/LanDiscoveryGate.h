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
 *
 * The start, advertise and stop callables never run under the gate's mutex: starting mDNS can block on the iOS alert,
 * and the gate must stay usable meanwhile (another thread's call, or a call from inside a callable, must not wait on
 * it). Whichever call finds work due becomes the one runner and runs the callables one at a time, in this order: a
 * stop, then a start, then the latest advertisement (so a start always precedes its advertisement, and the latest
 * advertisement wins). A call that arrives while a runner is busy only records its state and returns; the runner
 * picks it up before it ends. So a `Cancel()` during a start returns at once, and the stop runs right after the start
 * returns (a start that was cancelled is stopped, and its advertisement is dropped); a call from inside a callable
 * returns without running anything itself.
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
    std::unique_lock<std::mutex> lock(mu_);
    allowed_ = allowed;
    Run(lock);
  }

  /** Start discovery with `start` (run now when allowed, else held). Replaces an earlier request. */
  void Request(std::function<void()> start) {
    std::unique_lock<std::mutex> lock(mu_);
    CancelLocked();
    start_ = std::move(start);
    Run(lock);
  }

  /** The latest advertisement; sent now when discovery runs, else when it starts. */
  void Advertise(const std::string& peer_id, const int amp_udp_port) {
    std::unique_lock<std::mutex> lock(mu_);
    advertisement_ = std::make_pair(peer_id, amp_udp_port);
    advertisement_due_ = true;
    Run(lock);
  }

  /** Stops what runs and forgets what was held. */
  void Cancel() {
    std::unique_lock<std::mutex> lock(mu_);
    CancelLocked();
    Run(lock);
  }

  /** A start has been taken (it may still be running). */
  bool Started() const {
    std::lock_guard<std::mutex> lock(mu_);
    return started_;
  }

private:
  void CancelLocked() {
    if (started_) {
      stop_due_ = true; // after a start that is still running returns, if it is
    }
    started_ = false;
    start_ = nullptr;
    advertisement_.reset();
    advertisement_due_ = false;
  }

  /** The runner: runs what is due, unlocked, until nothing is. Does nothing while another runner is busy. */
  void Run(std::unique_lock<std::mutex>& lock) {
    if (running_) {
      return;
    }
    running_ = true;
    for (;;) {
      if (stop_due_) {
        stop_due_ = false;
        lock.unlock();
        if (ops_.stop) {
          ops_.stop();
        }
        lock.lock();
        continue;
      }
      if (allowed_ && !started_ && start_) {
        started_ = true; // "exactly once": a second SetAllowed(true) finds it taken
        std::function<void()> start = std::move(start_);
        start_ = nullptr;
        lock.unlock();
        start();
        lock.lock();
        continue;
      }
      if (started_ && advertisement_due_ && advertisement_) {
        advertisement_due_ = false;
        const auto advertisement = *advertisement_;
        lock.unlock();
        if (ops_.advertise) {
          ops_.advertise(advertisement.first, advertisement.second);
        }
        lock.lock();
        continue;
      }
      break;
    }
    running_ = false;
  }

  Ops ops_;
  mutable std::mutex mu_;
  bool allowed_ = true;
  bool started_ = false;
  bool running_ = false;
  bool stop_due_ = false;
  bool advertisement_due_ = false;
  std::function<void()> start_;
  std::optional<std::pair<std::string, int>> advertisement_;
};

} // namespace pbr
