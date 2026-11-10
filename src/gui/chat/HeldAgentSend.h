#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

/** How long a send waits for the assistant to finish connecting before it is given up. */
inline constexpr std::chrono::seconds kHeldAgentSendTimeout{15};

/**
 * Sends held while the assistant (re)configures, e.g. a Home chip tapped right after launch: one per thread, and a
 * later send in the same thread replaces it. Polled once per frame: they go out when the assistant is ready, or are
 * given up after kHeldAgentSendTimeout.
 */
class HeldAgentSend {
public:
  struct Send {
    std::string text;
    std::optional<std::string> payload;
    bool from_home_chip = false;
    std::string thread_id; // the thread it was typed in; it is not sent anywhere else
  };

  struct Polled {
    std::vector<Send> ready; // the assistant is ready: each goes out (or back), in the order they were held
    size_t expired = 0;      // given up
  };

  void Hold(Send send, const std::chrono::steady_clock::time_point now) {
    const std::string thread_id = send.thread_id;
    held_.erase(
        std::remove_if(held_.begin(), held_.end(), [&](const Held& held) { return held.send.thread_id == thread_id; }),
        held_.end());
    held_.push_back(Held{std::move(send), now + kHeldAgentSendTimeout});
  }

  bool Holding() const { return !held_.empty(); }
  bool Holding(const std::string& thread_id) const {
    return std::any_of(held_.begin(), held_.end(), [&](const Held& held) { return held.send.thread_id == thread_id; });
  }

  void Clear() { held_.clear(); }

  /** Ready: every held send leaves; otherwise those past their deadline are given up. */
  Polled Poll(const bool agent_ready, const std::chrono::steady_clock::time_point now) {
    Polled out;
    std::vector<Held> waiting;
    for (Held& held : held_) {
      if (agent_ready) {
        out.ready.push_back(std::move(held.send));
      } else if (now >= held.deadline) {
        ++out.expired;
      } else {
        waiting.push_back(std::move(held));
      }
    }
    held_ = std::move(waiting);
    return out;
  }

private:
  struct Held {
    Send send;
    std::chrono::steady_clock::time_point deadline;
  };
  std::vector<Held> held_; // in the order held
};

enum class HeldSendFate { Send, BackToDraft, Drop };

/**
 * Once the assistant is ready, a held send goes out only into the thread it was made in, and not over a reply
 * still loading there. Otherwise typed text goes back into the composer so it is not lost; a Home chip is
 * dropped (it can simply be tapped again).
 */
inline HeldSendFate HeldSendFateOf(const HeldAgentSend::Send& send, const std::string& active_thread_id,
                                   const bool reply_loading) {
  if (send.thread_id == active_thread_id && !reply_loading) {
    return HeldSendFate::Send;
  }
  return send.from_home_chip ? HeldSendFate::Drop : HeldSendFate::BackToDraft;
}

} // namespace pbr
