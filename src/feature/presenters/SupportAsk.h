#pragma once

#include "common/thread/ThreadTypes.h"
#include "feature/conversations/InboxController.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * The settings "?" asks PP 客服 in the half-screen panel (DEV/PLAN-help-to-support-panel.md): the panel shows the
 * PP 客服 chat, sends the "?"'s question into it and says it is waiting for the reply. These are its decisions, with
 * the time passed in; the panel (gui) and ChatController only carry them out.
 */

/** A message of the PP 客服 chat as these decisions see it. */
struct SupportMessage {
  bool own = false;
  std::string text;
  int64_t at_ms = 0;
  /** Ours: still being sent, did not go out, or out (None). */
  SendMark mark = SendMark::None;
};

/** The same question of ours this recently is not sent again (a second tap, the panel closed and opened again). */
inline constexpr int64_t kSupportRepeatWindowMs = 10 * 60 * 1000;
/** No reply this long after the question: the waiting line says PP 客服 got it and the reply will come here. */
inline constexpr int64_t kSupportSlowAfterMs = 90 * 1000;

/** What the panel's waiting line says. */
enum class SupportWait {
  Answered, // nothing of ours waits: a reply came after it (or nothing was asked)
  Waiting,  // "PP 客服正在回复…"
  Slow,     // "PP 客服收到了，回复来了会出现在这里" (still listening)
  NotSent,  // ours has not gone out: its own clock / red mark tells, no waiting line
};

/** `message` as one of the conversation (`mark`: its send mark); none for what is no message bubble (a system line). */
std::optional<SupportMessage> SupportMessageOf(const ThreadMessage& message, SendMark mark);

/**
 * Whether the "?"'s `question` goes out now: not when a message of ours with the same text was sent less than
 * kSupportRepeatWindowMs ago (whatever came after it), unless that one did not go out (Failed).
 */
bool ShouldAutoSend(const std::vector<SupportMessage>& messages, const std::string& question, int64_t now_ms);

/**
 * The waiting line for `messages` (oldest first) at `now_ms`: our questions after their last message wait for a
 * reply, timed from the first of them; any message of theirs after it answers them all (one reply to two questions
 * clears it: accepted, there is no question/answer pairing).
 */
SupportWait ReplyState(const std::vector<SupportMessage>& messages, int64_t now_ms);

/** A reply that arrived is read now: the panel shows its chat to someone looking at the app. */
bool ShouldReadSupportThread(bool attentive, bool panel_shows_thread);

/**
 * No notification for a reply in the chat the panel shows to someone looking (it was read there). The app notifies only
 * when nobody looks, so this guards the moment between; a phone put away with the panel open is notified.
 */
bool ShouldSuppressNotice(bool attentive, bool panel_shows_thread);

} // namespace pbr
