#include "lib/messaging/MentionAlert.h"

#include "lib/messaging/TextSpans.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

MentionVerdict MentionVerdictFor(const ThreadMessage& message, const std::string& group_id,
                                 const std::string& local_account, GroupRosterStore& roster, const bool sender_blocked) {
  if (group_id.empty() || local_account.empty() || message.content_type != ChatContentType::Text ||
      message.sender_contact_id == local_account || sender_blocked) {
    return MentionVerdict::No;
  }
  const std::vector<TextSpan> spans = text_spans::FromPayloadJson(message.payload_json);
  const std::vector<std::string> mentioned = text_spans::MentionedAccounts(spans);
  const bool names_us = std::find(mentioned.begin(), mentioned.end(), local_account) != mentioned.end();
  if (!names_us && !text_spans::MentionsEveryone(spans)) {
    return MentionVerdict::No;  // nothing that could reach us, whatever the roster says
  }
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata || !*metadata) {
    return MentionVerdict::Held;  // the roster is not known yet
  }
  if ((*metadata)->sync_state == GroupSyncState::Checking) {
    return MentionVerdict::Held;  // G013: the roster may be stale until the check settles
  }
  if ((*metadata)->sync_state != GroupSyncState::Active) {
    return MentionVerdict::No;  // removed
  }
  auto sender_in = roster.IsMember(group_id, message.sender_contact_id);
  auto we_are_in = roster.IsMember(group_id, local_account);
  if (!sender_in || !*sender_in || !we_are_in || !*we_are_in) {
    return MentionVerdict::No;
  }
  if (text_spans::MentionsEveryone(spans) && message.sender_contact_id == (*metadata)->owner_identity) {
    return MentionVerdict::Us;
  }
  return names_us ? MentionVerdict::Us : MentionVerdict::No;
}

bool MessageMentionsUs(const ThreadMessage& message, const std::string& group_id, const std::string& local_account,
                       GroupRosterStore& roster, const bool sender_blocked) {
  return MentionVerdictFor(message, group_id, local_account, roster, sender_blocked) == MentionVerdict::Us;
}

} // namespace pbr
