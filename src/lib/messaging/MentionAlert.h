#pragma once

#include "lib/messaging/GroupRosterStore.h"
#include "common/thread/ThreadTypes.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Whether a received group message mentions us (group-chat G015): a mention of our account, or `@everyone`
 * from the group's owner — the one the signed roster names (G012). Checked against the local roster now,
 * not at decode. Never for our own message or a blocked sender's, in a direct chat, in a group we were
 * removed from, nor while the group is still checking its members (G013: its roster may be stale).
 */
bool MessageMentionsUs(const ThreadMessage& message, const std::string& group_id, const std::string& local_account,
                       GroupRosterStore& roster, bool sender_blocked);

/**
 * A muted group still alerts on a mention, at most once per this many ms per group (G015): one member cannot
 * flood a muted group with mentions.
 */
inline constexpr int64_t kMutedMentionAlertGapMs = 60 * 1000;

/** The muted-group rate limit: alert when the last mention alert of this group is at least the gap ago. */
inline bool MutedMentionMayAlert(int64_t last_alert_ms, int64_t now_ms) {
  return last_alert_ms == 0 || now_ms - last_alert_ms >= kMutedMentionAlertGapMs;
}

/**
 * Whether a background notice alerts (G015): an unmuted chat always; a muted one only for a message that mentions
 * us — not when its mentions are muted too — and then once per kMutedMentionAlertGapMs at most (`last_alert_ms`,
 * this chat's last mention alert, is moved on when it does).
 */
inline bool NoticeAlerts(bool muted, bool mute_mentions, bool mentions_us, int64_t& last_alert_ms, int64_t now_ms) {
  if (!muted) {
    return true;
  }
  if (!mentions_us || mute_mentions || !MutedMentionMayAlert(last_alert_ms, now_ms)) {
    return false;
  }
  last_alert_ms = now_ms;
  return true;
}

} // namespace pbr
