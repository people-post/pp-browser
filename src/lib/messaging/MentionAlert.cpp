#include "lib/messaging/MentionAlert.h"

#include "lib/messaging/TextSpans.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

bool MessageMentionsUs(const ThreadMessage& message, const std::string& group_id, const std::string& local_account,
                       GroupRosterStore& roster, const bool sender_blocked) {
  if (group_id.empty() || local_account.empty() || message.content_type != ChatContentType::Text ||
      message.sender_contact_id == local_account || sender_blocked) {
    return false;
  }
  const std::vector<TextSpan> spans = text_spans::FromPayloadJson(message.payload_json);
  if (spans.empty()) {
    return false;
  }
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata || !*metadata || (*metadata)->sync_state != GroupSyncState::Active) {
    return false;
  }
  auto sender_in = roster.IsMember(group_id, message.sender_contact_id);
  auto we_are_in = roster.IsMember(group_id, local_account);
  if (!sender_in || !*sender_in || !we_are_in || !*we_are_in) {
    return false;
  }
  if (text_spans::MentionsEveryone(spans) && message.sender_contact_id == (*metadata)->owner_identity) {
    return true;
  }
  const std::vector<std::string> mentioned = text_spans::MentionedAccounts(spans);
  return std::find(mentioned.begin(), mentioned.end(), local_account) != mentioned.end();
}

} // namespace pbr
