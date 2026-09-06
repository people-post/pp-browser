#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/** Default redirect hops a viewer may follow before soft-fail (B007). */
inline constexpr int kDefaultBroadcastRedirectBudget = 8;
/** Max child PeerIds returned in one redirect (capacity-weighted pick later). */
inline constexpr size_t kDefaultBroadcastMaxRedirectHints = 3;
/** Cap L1 tip hints (publisher whitelist ∩ online). */
inline constexpr size_t kDefaultBroadcastMaxL1Hints = 16;

enum class BroadcastLadderViewerAction {
  /** Free viewer slot — attach as subscriber. */
  Admit = 0,
  /** No local slot — try whitelist∩online children. */
  Redirect = 1,
  /** Budget exhausted, loop, or no candidates. */
  Refuse = 2,
};

struct BroadcastLadderViewerDecision {
  BroadcastLadderViewerAction action = BroadcastLadderViewerAction::Refuse;
  std::vector<std::string> redirect_peer_ids;
  int redirect_budget_remaining = 0;
  std::string refuse_reason;
};

struct BroadcastLadderViewerInput {
  size_t free_viewer_slots = 0;
  /** Already filtered to this hop's help_media whitelist ∩ online. */
  std::vector<std::string> whitelist_online_children;
  int redirect_budget = kDefaultBroadcastRedirectBudget;
  /** PeerIds already visited on this join (loop defense). */
  std::vector<std::string> path_stamp;
  std::string self_peer_id;
  size_t max_redirect_hints = kDefaultBroadcastMaxRedirectHints;
  /**
   * Deterministic jitter in [0, 1). Rotates which child is listed first.
   * 0 keeps whitelist order.
   */
  double jitter_unit = 0.0;
};

/**
 * B007 admit-or-redirect for a viewer presenting a valid join ticket.
 * Pure — no I/O. Same policy at publisher L1 and deeper hops.
 */
BroadcastLadderViewerDecision DecideBroadcastViewerAdmit(const BroadcastLadderViewerInput& in);

enum class BroadcastLadderSlotWinAction {
  /** Parent has a free child slot — admit relay. */
  AdmitRelay = 0,
  /** At degree pressure — demote viewer(s) one rung, then admit relay. */
  DemoteViewersAndAdmitRelay = 1,
  Refuse = 2,
};

struct BroadcastLadderSlotWinDecision {
  BroadcastLadderSlotWinAction action = BroadcastLadderSlotWinAction::Refuse;
  std::vector<std::string> demote_viewer_peer_ids;
  /** Prefer redirect demoted viewers onto the new relay. */
  std::string demotion_redirect_target;
  std::string refuse_reason;
};

struct BroadcastLadderSlotWinInput {
  size_t free_child_slots = 0;
  bool candidate_on_whitelist = false;
  bool slot_win_rate_limited = false;
  /** Viewers currently piped on this parent (demote priority order). */
  std::vector<std::string> demotable_viewer_peer_ids;
  std::string new_relay_peer_id;
  size_t max_demotions = 1;
};

/**
 * B007 slot-win: whitelist relay may claim a child slot; demote piped viewers
 * one rung (grace redirect) when the parent is full.
 */
BroadcastLadderSlotWinDecision DecideBroadcastSlotWin(const BroadcastLadderSlotWinInput& in);

/**
 * Preserve whitelist order; keep only ids present in `online_peer_ids`; skip empty;
 * dedupe; cap at max_hints.
 */
std::vector<std::string> FilterOnlineWhitelist(const std::vector<std::string>& whitelist,
                                               const std::vector<std::string>& online_peer_ids,
                                               size_t max_hints = kDefaultBroadcastMaxL1Hints);

/** Dial target: explicit hop_peer_id, else first L1 hint. */
std::string PrimaryBroadcastHopPeerId(const std::string& hop_peer_id,
                                      const std::vector<std::string>& l1_hop_peer_ids);

} // namespace pbr
