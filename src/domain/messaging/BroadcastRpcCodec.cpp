#include "domain/messaging/BroadcastRpcCodec.h"

#include "common/ValueJson.h"

#include <utility>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

Value StringArrayValue(const std::vector<std::string>& ids) {
  std::vector<Value> items;
  items.reserve(ids.size());
  for (const std::string& id : ids) {
    if (!id.empty()) {
      items.emplace_back(id);
    }
  }
  return ArrayValue(std::move(items));
}

std::vector<std::string> ReadStringArray(const Object& o, const char* key) {
  std::vector<std::string> out;
  if (const Array* arr = ObjectArray(o, key)) {
    for (const auto& item : arr->elements) {
      if (auto s = asString(item)) {
        if (!s->empty()) {
          out.push_back(*s);
        }
      }
    }
  }
  return out;
}

const char* ViewerActionToWire(BroadcastLadderViewerAction action) {
  switch (action) {
  case BroadcastLadderViewerAction::Admit:
    return "admit";
  case BroadcastLadderViewerAction::Redirect:
    return "redirect";
  case BroadcastLadderViewerAction::Refuse:
    return "refuse";
  }
  return "refuse";
}

std::optional<BroadcastLadderViewerAction> ViewerActionFromWire(std::string_view value) {
  if (value == "admit") {
    return BroadcastLadderViewerAction::Admit;
  }
  if (value == "redirect") {
    return BroadcastLadderViewerAction::Redirect;
  }
  if (value == "refuse") {
    return BroadcastLadderViewerAction::Refuse;
  }
  return std::nullopt;
}

const char* SlotWinActionToWire(BroadcastLadderSlotWinAction action) {
  switch (action) {
  case BroadcastLadderSlotWinAction::AdmitRelay:
    return "admit_relay";
  case BroadcastLadderSlotWinAction::DemoteViewersAndAdmitRelay:
    return "demote_and_admit_relay";
  case BroadcastLadderSlotWinAction::Refuse:
    return "refuse";
  }
  return "refuse";
}

std::optional<BroadcastLadderSlotWinAction> SlotWinActionFromWire(std::string_view value) {
  if (value == "admit_relay") {
    return BroadcastLadderSlotWinAction::AdmitRelay;
  }
  if (value == "demote_and_admit_relay") {
    return BroadcastLadderSlotWinAction::DemoteViewersAndAdmitRelay;
  }
  if (value == "refuse") {
    return BroadcastLadderSlotWinAction::Refuse;
  }
  return std::nullopt;
}

} // namespace

Roe<std::string> EncodeBroadcastTicketRequest(const BroadcastTicketRequest& req) {
  Object root;
  root.set("op", kBroadcastOpTicketRequest);
  root.set("program_id", req.program_id);
  root.set("join_handle", req.join_handle);
  root.set("viewer_peer_id", req.viewer_peer_id);
  return DumpJson(root);
}

Roe<std::string> EncodeBroadcastTicketResponse(const BroadcastTicketResponse& resp) {
  Object root;
  root.set("op", kBroadcastOpTicketResponse);
  root.set("ok", resp.ok);
  if (!resp.error.empty()) {
    root.set("error", resp.error);
  }
  if (resp.ticket) {
    auto ticket_json = EncodeBroadcastJoinTicketJson(*resp.ticket);
    if (!ticket_json) {
      return ticket_json.error();
    }
    auto ticket_obj = ParseObject(*ticket_json);
    if (!ticket_obj) {
      return ticket_obj.error();
    }
    root.set("ticket", *ticket_obj);
  }
  return DumpJson(root);
}

Roe<std::string> EncodeBroadcastViewerAttachRequest(const BroadcastViewerAttachRequest& req) {
  Object root;
  root.set("op", kBroadcastOpViewerAttach);
  root.set("program_id", req.program_id);
  root.set("join_handle", req.join_handle);
  root.set("viewer_peer_id", req.viewer_peer_id);
  root.set("ticket_json", req.ticket_json);
  root.set("redirect_budget", static_cast<int64_t>(req.redirect_budget));
  if (!req.path_stamp.empty()) {
    root.set("path_stamp", StringArrayValue(req.path_stamp));
  }
  return DumpJson(root);
}

Roe<std::string> EncodeBroadcastViewerAttachResult(const BroadcastViewerAttachResult& result) {
  Object root;
  root.set("op", kBroadcastOpViewerAttachResult);
  root.set("action", ViewerActionToWire(result.action));
  if (!result.redirect_peer_ids.empty()) {
    root.set("redirect_peer_ids", StringArrayValue(result.redirect_peer_ids));
  }
  root.set("redirect_budget_remaining", static_cast<int64_t>(result.redirect_budget_remaining));
  if (!result.refuse_reason.empty()) {
    root.set("refuse_reason", result.refuse_reason);
  }
  if (!result.admitted_hop_peer_id.empty()) {
    root.set("admitted_hop_peer_id", result.admitted_hop_peer_id);
  }
  return DumpJson(root);
}

Roe<std::string> EncodeBroadcastRelaySlotWinRequest(const BroadcastRelaySlotWinRequest& req) {
  Object root;
  root.set("op", kBroadcastOpRelaySlotWin);
  root.set("program_id", req.program_id);
  root.set("join_handle", req.join_handle);
  root.set("relay_peer_id", req.relay_peer_id);
  return DumpJson(root);
}

Roe<std::string> EncodeBroadcastRelaySlotWinResult(const BroadcastRelaySlotWinResult& result) {
  Object root;
  root.set("op", kBroadcastOpRelaySlotWinResult);
  root.set("action", SlotWinActionToWire(result.action));
  if (!result.demote_viewer_peer_ids.empty()) {
    root.set("demote_viewer_peer_ids", StringArrayValue(result.demote_viewer_peer_ids));
  }
  if (!result.demotion_redirect_target.empty()) {
    root.set("demotion_redirect_target", result.demotion_redirect_target);
  }
  if (!result.refuse_reason.empty()) {
    root.set("refuse_reason", result.refuse_reason);
  }
  return DumpJson(root);
}

Roe<BroadcastRpcMessage> DecodeBroadcastRpcJson(const std::string_view json) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return parsed.error();
  }
  const Object& root = *parsed;
  const std::string op = ObjectString(root, "op").value_or("");

  if (op == kBroadcastOpTicketRequest) {
    BroadcastTicketRequest req;
    req.program_id = ObjectString(root, "program_id").value_or("");
    req.join_handle = ObjectString(root, "join_handle").value_or("");
    req.viewer_peer_id = ObjectString(root, "viewer_peer_id").value_or("");
    return BroadcastRpcMessage{std::move(req)};
  }

  if (op == kBroadcastOpTicketResponse) {
    BroadcastTicketResponse resp;
    resp.ok = ObjectBool(root, "ok").value_or(false);
    resp.error = ObjectString(root, "error").value_or("");
    if (const Object* ticket_obj = ObjectChild(root, "ticket")) {
      auto ticket = DecodeBroadcastJoinTicketJson(DumpJson(*ticket_obj));
      if (!ticket) {
        return ticket.error();
      }
      resp.ticket = std::move(*ticket);
    }
    return BroadcastRpcMessage{std::move(resp)};
  }

  if (op == kBroadcastOpViewerAttach) {
    BroadcastViewerAttachRequest req;
    req.program_id = ObjectString(root, "program_id").value_or("");
    req.join_handle = ObjectString(root, "join_handle").value_or("");
    req.viewer_peer_id = ObjectString(root, "viewer_peer_id").value_or("");
    req.ticket_json = ObjectString(root, "ticket_json").value_or("");
    req.redirect_budget = static_cast<int>(
        ObjectInt64(root, "redirect_budget").value_or(kDefaultBroadcastRedirectBudget));
    req.path_stamp = ReadStringArray(root, "path_stamp");
    return BroadcastRpcMessage{std::move(req)};
  }

  if (op == kBroadcastOpViewerAttachResult) {
    BroadcastViewerAttachResult result;
    const auto action = ViewerActionFromWire(ObjectString(root, "action").value_or(""));
    if (!action) {
      return Error("broadcast viewer_attach_result missing/invalid action");
    }
    result.action = *action;
    result.redirect_peer_ids = ReadStringArray(root, "redirect_peer_ids");
    result.redirect_budget_remaining =
        static_cast<int>(ObjectInt64(root, "redirect_budget_remaining").value_or(0));
    result.refuse_reason = ObjectString(root, "refuse_reason").value_or("");
    result.admitted_hop_peer_id = ObjectString(root, "admitted_hop_peer_id").value_or("");
    return BroadcastRpcMessage{std::move(result)};
  }

  if (op == kBroadcastOpRelaySlotWin) {
    BroadcastRelaySlotWinRequest req;
    req.program_id = ObjectString(root, "program_id").value_or("");
    req.join_handle = ObjectString(root, "join_handle").value_or("");
    req.relay_peer_id = ObjectString(root, "relay_peer_id").value_or("");
    return BroadcastRpcMessage{std::move(req)};
  }

  if (op == kBroadcastOpRelaySlotWinResult) {
    BroadcastRelaySlotWinResult result;
    const auto action = SlotWinActionFromWire(ObjectString(root, "action").value_or(""));
    if (!action) {
      return Error("broadcast relay_slot_win_result missing/invalid action");
    }
    result.action = *action;
    result.demote_viewer_peer_ids = ReadStringArray(root, "demote_viewer_peer_ids");
    result.demotion_redirect_target = ObjectString(root, "demotion_redirect_target").value_or("");
    result.refuse_reason = ObjectString(root, "refuse_reason").value_or("");
    return BroadcastRpcMessage{std::move(result)};
  }

  return Error("unsupported broadcast rpc op");
}

BroadcastViewerAttachResult BroadcastViewerAttachResultFromDecision(
    const BroadcastLadderViewerDecision& decision, const std::string_view self_peer_id) {
  BroadcastViewerAttachResult out;
  out.action = decision.action;
  out.redirect_peer_ids = decision.redirect_peer_ids;
  out.redirect_budget_remaining = decision.redirect_budget_remaining;
  out.refuse_reason = decision.refuse_reason;
  if (decision.action == BroadcastLadderViewerAction::Admit) {
    out.admitted_hop_peer_id = std::string(self_peer_id);
  }
  return out;
}

BroadcastRelaySlotWinResult BroadcastRelaySlotWinResultFromDecision(
    const BroadcastLadderSlotWinDecision& decision) {
  BroadcastRelaySlotWinResult out;
  out.action = decision.action;
  out.demote_viewer_peer_ids = decision.demote_viewer_peer_ids;
  out.demotion_redirect_target = decision.demotion_redirect_target;
  out.refuse_reason = decision.refuse_reason;
  return out;
}

} // namespace pbr
