#pragma once

#include "domain/messaging/CallTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/**
 * Pure hop SoftMigrate / SFU-attach helpers (fan-out shape, attach-wait poll, hop-hint).
 * CallTopologyController / CallHopMigrateWorkflow own IO + posting.
 */

// --- Fan-out ---

/**
 * Shape CallSfuAttach for fan-out after the picker finished AcceptAndAttach.
 * Clears quote_id — hop quotes are single-use; peers must RequestQuote themselves.
 */
CallSfuAttachDetail BuildSfuAttachFanout(const CallSfuAttachDetail& after_local_attach);

/** FNV-1a style publisher stream id from communicating identity (stable across peers). */
uint32_t PublisherStreamIdForIdentity(const std::string& identity);

// --- Attach-wait ---

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
  bool media_active_mesh_for_call = false;
};

/**
 * Pure attach-wait poll (V021 / V025).
 * No TimeoutLeave while soft_migrate_in_flight — avoids chrome wipe mid-migrate.
 */
SfuAttachWaitPollResult PollSfuAttachWait(const SfuAttachWaitPollInput& in);

// --- Hop hint (guest prefs → owner re-pick) ---

/** Max hop PeerIds a guest may suggest after SFU attach failure (V029). */
inline constexpr size_t kMaxGuestHopPreferences = 5;

enum class HopHintOwnerAction {
  /** Intersection non-empty — SoftMigrate with preferred hop first. */
  RePick = 0,
  /** No shared durable hop — refuse / eject guest. */
  RefuseGuest = 1,
};

struct HopHintOwnerDecision {
  HopHintOwnerAction action = HopHintOwnerAction::RefuseGuest;
  /** PeerId to try first when action == RePick. */
  std::string preferred_hop_peer_id;
};

/**
 * Owner reaction to guest CallSfuAttachFailed preferences (V029).
 * Picks the first guest preference that appears in owner_ranked_dialable (excluding failed_hop).
 * Empty intersection → RefuseGuest.
 */
HopHintOwnerDecision DecideHopHintOwnerAction(const std::vector<std::string>& guest_preferred_peer_ids,
                                              const std::vector<std::string>& owner_ranked_dialable_peer_ids,
                                              const std::string& failed_hop_peer_id);

/**
 * Truncate + dedupe guest prefs for wire (stable order, skip empty / failed hop).
 */
std::vector<std::string> CapGuestHopPreferences(std::vector<std::string> preferred,
                                                const std::string& failed_hop_peer_id,
                                                size_t max_prefs = kMaxGuestHopPreferences);

} // namespace pbr
