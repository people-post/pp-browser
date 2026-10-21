#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace pbr {

/** How long a send waits for the assistant to finish connecting before it is given up. */
inline constexpr std::chrono::seconds kHeldAgentSendTimeout{15};

/**
 * The one send held while the assistant (re)configures, e.g. a Home chip tapped right after launch. A later
 * send replaces it. Polled once per frame: it goes out when the assistant is ready, or is dropped after
 * kHeldAgentSendTimeout.
 */
class HeldAgentSend {
public:
  struct Send {
    std::string text;
    std::optional<std::string> payload;
    bool from_home_chip = false;
    std::string thread_id; // the thread it was typed in; it is not sent anywhere else
  };

  enum class Step { Idle, Wait, Send, Expired };

  void Hold(Send send, const std::chrono::steady_clock::time_point now) {
    held_ = std::move(send);
    deadline_ = now + kHeldAgentSendTimeout;
  }

  bool Holding() const { return held_.has_value(); }

  void Clear() { held_.reset(); }

  /** Send: `out` is the held send and the slot is empty. Expired: the slot is empty. */
  Step Poll(const bool agent_ready, const std::chrono::steady_clock::time_point now, Send& out) {
    if (!held_) {
      return Step::Idle;
    }
    if (agent_ready) {
      out = std::move(*held_);
      held_.reset();
      return Step::Send;
    }
    if (now >= deadline_) {
      held_.reset();
      return Step::Expired;
    }
    return Step::Wait;
  }

private:
  std::optional<Send> held_;
  std::chrono::steady_clock::time_point deadline_{};
};

} // namespace pbr
