#include "domain/messaging/SfuAttachWaitLogic.h"

namespace pbr {

SfuAttachWaitPollResult PollSfuAttachWait(const SfuAttachWaitPollInput& in) {
  if (!in.wait_active) {
    return SfuAttachWaitPollResult::Idle;
  }
  if (in.sfu_attached_for_call) {
    return SfuAttachWaitPollResult::ClearAttached;
  }
  // 1:1 P2P may still be connecting — never convert into a group-relay timeout leave.
  if (in.joined_count < 3 && in.media_active_mesh_for_call) {
    return SfuAttachWaitPollResult::ClearAsP2p;
  }
  if (in.soft_migrate_in_flight) {
    return SfuAttachWaitPollResult::Waiting;
  }
  if (in.now_ms < in.deadline_ms) {
    return SfuAttachWaitPollResult::Waiting;
  }
  return SfuAttachWaitPollResult::TimeoutLeave;
}

} // namespace pbr
