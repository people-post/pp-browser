#pragma once

#include <atomic>

namespace pbr {

/**
 * Keeps this device's push binding off the running backend while a switch to another one awaits restart.
 * The old profile's signing key is only usable in this run, so the binding is removed now, not at the next start.
 */
class PushBindingGate {
public:
  enum class Action { None, Unbind, Rebind };

  /** Registration is held back from the moment a switch is pending until it is undone. */
  bool Suspended() const { return suspended_; }

  /** The saved backend now differs from the running one (`pending`) or is the running one again. */
  Action OnSwitchPending(const bool pending) {
    if (pending) {
      suspended_ = true;
      return unbound_ ? Action::None : Action::Unbind;
    }
    if (!suspended_) {
      return Action::None;
    }
    suspended_ = false;
    const bool was_unbound = unbound_.exchange(false);
    return was_unbound ? Action::Rebind : Action::None;
  }

  /**
   * Outcome of an Unbind; a failed one is retried by the next pending switch save.
   * Returns true when the switch was undone while the Unbind ran, so the caller must register again.
   */
  bool OnUnbound(const bool ok) {
    unbound_ = ok;
    if (ok && !suspended_) {
      unbound_ = false;
      return true;
    }
    return false;
  }

private:
  std::atomic<bool> suspended_{false};
  std::atomic<bool> unbound_{false};
};

} // namespace pbr
