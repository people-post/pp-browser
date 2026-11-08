#pragma once

#include "feature/conversations/GroupInviteLinkWorkflow.h"

#include "common/Error.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace pbr {

/** What the join prompt needs. UI thread; `ask` and `confirm` run on a worker and answer here. */
struct GroupJoinPromptPorts {
  std::function<void(const std::string& link, std::function<void(Roe<std::string> request_id)> done)> ask;
  std::function<std::optional<GroupJoinState>(const std::string& request_id)> state;
  std::function<void(const std::string& request_id, std::function<void(Roe<Thread>)> done)> confirm;
  std::function<void(const std::string& request_id)> cancel;
  std::function<void(const std::string& title, const std::string& message, const std::string& ok_label,
                     std::function<void(bool)> done)>
      show_confirm;
  std::function<void(const std::string& text)> toast;
  std::function<void(const std::string& thread_id)> open_thread;
  /** An account's public name (the link's maker, before any chat with them). */
  std::function<std::string(const std::string& account)> name_of;
};

/**
 * Joining by an invite link (G021) from wherever one arrives — opened from the OS, pasted into Add contact, scanned:
 * asks the link's maker, then shows the preview ("Join "{name}"? …") with Join / Cancel, or says in plain words why
 * not; Join opens the group's chat. One join at a time. UI thread.
 */
class GroupJoinPrompt {
public:
  explicit GroupJoinPrompt(GroupJoinPromptPorts ports) : ports_(std::move(ports)) {}

  /** `text` holds a `pp://join/…` link: asks to join by it and returns true; anything else is not ours (false). */
  bool Offer(const std::string& text);
  /** Per frame: follows the join in flight (its answer, or none). */
  void Tick(std::chrono::steady_clock::time_point now);

private:
  void Asked(const std::string& maker, Roe<std::string> request_id);

  GroupJoinPromptPorts ports_;
  std::string request_id_;
  std::string maker_;
  std::chrono::steady_clock::time_point last_look_{};
};

} // namespace pbr
