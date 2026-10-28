#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace pbr {

/**
 * The AI runs one turn at a time (one AgentSession). "Busy" belongs to the thread the turn runs in: only that
 * thread shows "thinking" and the stop key; other threads keep sending (B112, DEV/PLAN-ai-per-thread-busy.md).
 */
struct AgentBusyState {
  bool busy = false;
  std::string thread_id; // empty while busy: the turn's thread is unknown, treat every thread as busy

  bool BusyIn(const std::string& thread) const { return busy && (thread_id.empty() || thread_id == thread); }
  bool BusyElsewhere(const std::string& thread) const { return busy && !thread_id.empty() && thread_id != thread; }
};

enum class SendDecision {
  Send,        // goes out now
  Queue,       // an AI send while another thread's turn runs: kept and sent when the AI is free
  BlockedHere, // an AI send in the thread whose turn is running (the composer shows the stop key)
  AlreadyQueued, // this thread already has a queued AI send: the text stays in the composer
};

/**
 * `is_ai_send` comes from MessageRouter::ExpectsAgentWork (AI thread, "@ai…", agent payloads); a message that
 * does not use the AI always goes out, also in the thread whose turn is running.
 */
inline SendDecision DecideSend(const bool is_ai_send, const AgentBusyState& busy, const std::string& target_thread,
                               const bool target_has_queued) {
  if (!is_ai_send) {
    return SendDecision::Send;
  }
  if (target_has_queued) {
    return SendDecision::AlreadyQueued;
  }
  if (!busy.busy) {
    return SendDecision::Send;
  }
  if (busy.thread_id.empty()) {
    return SendDecision::Queue; // unknown thread: never drop, queue
  }
  return busy.thread_id == target_thread ? SendDecision::BlockedHere : SendDecision::Queue;
}

enum class EscapeAction { None, CancelQueued, StopTurn };

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
  return EscapeAction::None;
}

/** What happened when a queued send was handed over. */
enum class QueuedSendOutcome {
  Started,    // a turn began: wait for it to end before the next one
  NotStarted, // nothing to run (thread gone, route failed, not an AI send): go on with the next one
  Busy,       // the AI turned busy first: put it back at the head and wait
};

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
