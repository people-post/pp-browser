#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pbr {

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
