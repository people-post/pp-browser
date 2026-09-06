#pragma once

#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/BroadcastLadderLogic.h"

#include "common/chat/IDirectMessageClient.h"
#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/** Spine F control plane — ticket mint + ladder admit/redirect + slot-win (protocol id: kRpcBroadcastProtocolId). */

inline constexpr const char* kBroadcastOpTicketRequest = "ticket_request";
inline constexpr const char* kBroadcastOpTicketResponse = "ticket_response";
inline constexpr const char* kBroadcastOpViewerAttach = "viewer_attach";
inline constexpr const char* kBroadcastOpViewerAttachResult = "viewer_attach_result";
inline constexpr const char* kBroadcastOpRelaySlotWin = "relay_slot_win";
inline constexpr const char* kBroadcastOpRelaySlotWinResult = "relay_slot_win_result";

struct BroadcastTicketRequest {
  std::string program_id;
  std::string join_handle;
  std::string viewer_peer_id;
};

struct BroadcastTicketResponse {
  bool ok = false;
  std::string error;
  std::optional<BroadcastJoinTicket> ticket;
};

struct BroadcastViewerAttachRequest {
  std::string program_id;
  std::string join_handle;
  std::string viewer_peer_id;
  /** Opaque ticket JSON (`EncodeBroadcastJoinTicketJson`). */
  std::string ticket_json;
  int redirect_budget = kDefaultBroadcastRedirectBudget;
  std::vector<std::string> path_stamp;
};

struct BroadcastViewerAttachResult {
  BroadcastLadderViewerAction action = BroadcastLadderViewerAction::Refuse;
  std::vector<std::string> redirect_peer_ids;
  int redirect_budget_remaining = 0;
  std::string refuse_reason;
  /** Hop that admitted (self) when action == Admit. */
  std::string admitted_hop_peer_id;
};

struct BroadcastRelaySlotWinRequest {
  std::string program_id;
  std::string join_handle;
  std::string relay_peer_id;
};

struct BroadcastRelaySlotWinResult {
  BroadcastLadderSlotWinAction action = BroadcastLadderSlotWinAction::Refuse;
  std::vector<std::string> demote_viewer_peer_ids;
  std::string demotion_redirect_target;
  std::string refuse_reason;
};

using BroadcastRpcMessage =
    std::variant<BroadcastTicketRequest, BroadcastTicketResponse, BroadcastViewerAttachRequest,
                 BroadcastViewerAttachResult, BroadcastRelaySlotWinRequest, BroadcastRelaySlotWinResult>;

Roe<std::string> EncodeBroadcastTicketRequest(const BroadcastTicketRequest& req);
Roe<std::string> EncodeBroadcastTicketResponse(const BroadcastTicketResponse& resp);
Roe<std::string> EncodeBroadcastViewerAttachRequest(const BroadcastViewerAttachRequest& req);
Roe<std::string> EncodeBroadcastViewerAttachResult(const BroadcastViewerAttachResult& result);
Roe<std::string> EncodeBroadcastRelaySlotWinRequest(const BroadcastRelaySlotWinRequest& req);
Roe<std::string> EncodeBroadcastRelaySlotWinResult(const BroadcastRelaySlotWinResult& result);

Roe<BroadcastRpcMessage> DecodeBroadcastRpcJson(std::string_view json);

BroadcastViewerAttachResult BroadcastViewerAttachResultFromDecision(
    const BroadcastLadderViewerDecision& decision, std::string_view self_peer_id);

BroadcastRelaySlotWinResult BroadcastRelaySlotWinResultFromDecision(
    const BroadcastLadderSlotWinDecision& decision);

} // namespace pbr
