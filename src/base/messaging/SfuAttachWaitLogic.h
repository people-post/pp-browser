#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/** Default attach-wait budget (must outlast multi-hop quote/attach IO). */
inline constexpr int64_t kSfuAttachWaitDefaultMs = 45000;

enum class SfuAttachWaitPollResult {
  Idle = 0,
  Waiting = 1,
  ClearAttached = 2,
  ClearAsP2p = 3,
  TimeoutLeave = 4,
};

struct SfuAttachWaitPollInput {
  bool wait_active = false;
  int64_t now_ms = 0;
  int64_t deadline_ms = 0;
  /** SoftMigrate / AttachLocal still running — never TimeoutLeave. */
  bool soft_migrate_in_flight = false;
  bool sfu_attached_for_call = false;
  size_t joined_count = 0;
  /** Active 1:1 P2P for this call_id (not SFU). */
  bool media_active_p2p_for_call = false;
};

/**
 * Pure attach-wait poll (V021 / V025).
 * No TimeoutLeave while soft_migrate_in_flight — avoids chrome wipe mid-migrate.
 */
SfuAttachWaitPollResult PollSfuAttachWait(const SfuAttachWaitPollInput& in);

} // namespace pbr
