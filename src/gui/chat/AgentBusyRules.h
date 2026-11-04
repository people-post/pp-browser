#pragma once

#include "feature/ai/AgentSessions.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>

namespace pbr {

/**
 * The threads whose AI turn runs now: AgentSessions answers up to its cap (kMaxConcurrentTurns; a guest 1) at once.
 * Only those threads show "thinking" and the stop key (B112). Whether another AI send may start is the sessions'
 * answer (can_start), never counted here (DEV/PLAN-ai-concurrent-turns.md).
 */
struct AgentBusyState {
  std::set<std::string> threads;

  bool Any() const { return !threads.empty(); }
  bool BusyIn(const std::string& thread) const { return threads.count(thread) > 0; }
};

enum class SendDecision {
  Send,          // goes out now
  Queue,         // an AI send while as many chats as allowed are answered: kept and sent when one ends
  BlockedHere,   // an AI send in a thread whose turn is running (the composer shows the stop key)
  AlreadyQueued, // this thread already has a queued AI send: the text stays in the composer
};

/**
 * `is_ai_send` comes from MessageRouter::ExpectsAgentWork (AI thread, "@ai…", agent payloads); a message that
 * does not use the AI always goes out, also in a thread whose turn is running. `can_start` is the sessions' answer
 * for the target thread; `busy_here`: the screen still shows a turn there (its end not heard yet).
 */
inline SendDecision DecideSend(const bool is_ai_send, const TurnStart can_start, const bool busy_here,
                               const bool target_has_queued) {
  if (!is_ai_send) {
    return SendDecision::Send;
  }
  if (target_has_queued) {
    return SendDecision::AlreadyQueued;
  }
  if (busy_here || can_start == TurnStart::BusyHere) {
    return SendDecision::BlockedHere;
  }
  return can_start == TurnStart::Full ? SendDecision::Queue : SendDecision::Send;
}

enum class EscapeAction {
  None,         // nothing for the composer: Escape dismisses / goes back as usual
  CancelQueued, // this chat's queued AI question is cancelled
  Armed,        // a turn runs here: the first press only arms the stop (and is used up, so it does not navigate away)
  StopTurn,     // the second press within the window stops it
};

inline constexpr int64_t kDoubleEscapeWindowMs = 500;

/**
 * Escape with the composer focused (desktop). Overlays (menus, dialogs, the emoji panel) are closed by the shell
 * before this is asked. Once: cancel this thread's queued AI send. Twice within kDoubleEscapeWindowMs: stop the
 * turn running in this thread. Otherwise nothing (the draft is never cleared by Escape).
 */
inline EscapeAction DecideEscape(const int64_t now_ms, const std::optional<int64_t> last_escape_ms,
                                 const bool has_queued_here, const bool busy_here) {
  const bool second_press = last_escape_ms && now_ms >= *last_escape_ms && now_ms - *last_escape_ms <= kDoubleEscapeWindowMs;
  if (second_press && busy_here) {
    return EscapeAction::StopTurn;
  }
  if (has_queued_here) {
    return EscapeAction::CancelQueued;
  }
  return busy_here ? EscapeAction::Armed : EscapeAction::None;
}

/** What happened when a queued send was handed over. */
enum class QueuedSendOutcome {
  Started,    // a turn began: go on with the next one while the sessions can start it
  NotStarted, // nothing to run (thread gone, route failed, not an AI send): go on with the next one
  Busy,       // the AI turned busy first: put it back at the head and wait
};

/**
 * The user is looking at `thread_id`'s messages now: it is the open thread and its page is on screen (compact:
 * the chat page is open over the list; expanded: the Sessions tab with the chat pane).
 */
inline bool ChatPageShows(const ShellChromeSnapshot& chrome, const std::string& active_thread,
                          const std::string& thread_id) {
  if (thread_id.empty() || thread_id != active_thread || chrome.nav_tab == NavTab::Home) {
    return false;
  }
  if (chrome.layout_mode == LayoutMode::Compact) {
    return chrome.compact_chat_open;
  }
  return chrome.nav_tab == NavTab::Sessions && chrome.primary_pane_key == "chat";
}

/** One queued AI send. Shown as a dimmed "me" bubble at the end of its thread; stored and sent only when it runs. */
struct QueuedAgentSend {
  std::string thread_id;
  std::string text;
  std::optional<std::string> payload;
  bool shared_confirmed = false;        // "@ai+" / "@ai++" already confirmed before it was queued
  bool thread_created_for_this = false; // a Home question opened this thread; cancelling removes it if still empty
};

/** First in, first out; at most one item per thread. In memory only (lost on quit, logged). */
class AgentSendQueue {
public:
  bool Enqueue(QueuedAgentSend item) {
    if (Has(item.thread_id)) {
      return false;
    }
    items_.push_back(std::move(item));
    return true;
  }
  /** Put a send that was refused (the AI turned busy) back at the head, so order is kept. */
  void PushFront(QueuedAgentSend item) {
    if (!Has(item.thread_id)) {
      items_.push_front(std::move(item));
    }
  }
  bool Has(const std::string& thread_id) const { return Find(thread_id) != nullptr; }
  const QueuedAgentSend* Find(const std::string& thread_id) const {
    for (const QueuedAgentSend& item : items_) {
      if (item.thread_id == thread_id) {
        return &item;
      }
    }
    return nullptr;
  }
  std::optional<QueuedAgentSend> Take(const std::string& thread_id) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (it->thread_id == thread_id) {
        QueuedAgentSend item = std::move(*it);
        items_.erase(it);
        return item;
      }
    }
    return std::nullopt;
  }
  std::optional<QueuedAgentSend> PopFront() {
    if (items_.empty()) {
      return std::nullopt;
    }
    QueuedAgentSend item = std::move(items_.front());
    items_.pop_front();
    return item;
  }
  bool Empty() const { return items_.empty(); }
  size_t Size() const { return items_.size(); }

private:
  std::deque<QueuedAgentSend> items_;
};

} // namespace pbr
