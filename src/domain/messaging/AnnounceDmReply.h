#pragma once

#include "common/Error.h"

#include "common/PbrCompat.h"
#include "common/thread/ThreadRecordTypes.h"

#include <string>
#include <string_view>

namespace pbr {

/** Open a DM thread for a tip reply (no in-topic announce speak). */
struct AnnounceDmReplyPlan {
  DirectChatTarget target;
  std::string contact_id;
  std::string thread_title;
};

/**
 * Plan a DM reply to `tip_peer_id` (Spine B speak/reply rule).
 *
 * Prefer `contact_account_id` as the product thread key when set; otherwise
 * key the thread by PeerId. Never invents epidemic/helper fan-out.
 */
Roe<AnnounceDmReplyPlan> PlanAnnounceDmReply(std::string_view tip_peer_id, std::string_view contact_id,
                                             std::string_view contact_account_id,
                                             std::string_view thread_title);

} // namespace pbr
