#include "domain/messaging/AnnounceDmReply.h"

#include "common/directory/DirectoryJson.h"
#include "common/thread/ThreadChannel.h"

namespace pbr {

Roe<AnnounceDmReplyPlan> PlanAnnounceDmReply(const std::string_view tip_peer_id,
                                             const std::string_view contact_id,
                                             const std::string_view contact_account_id,
                                             const std::string_view thread_title) {
  if (tip_peer_id.empty()) {
    return Error("Missing announce publisher peer_id");
  }

  AnnounceDmReplyPlan plan;
  plan.contact_id = std::string(contact_id);
  plan.thread_title = thread_title.empty() ? std::string(tip_peer_id) : std::string(thread_title);
  plan.target.channel = ThreadChannel::E2ePublic;

  if (!contact_account_id.empty()) {
    plan.target.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
    plan.target.peer_identity_value = std::string(contact_account_id);
  } else {
    plan.target.peer_identity_kind = ContactIdKindToString(ContactIdKind::PeerId);
    plan.target.peer_identity_value = std::string(tip_peer_id);
  }
  return plan;
}

} // namespace pbr
