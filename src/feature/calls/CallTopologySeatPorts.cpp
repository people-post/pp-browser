#include "feature/calls/CallTopologySeatPorts.h"

namespace pbr {

CallTopologySeatPorts MakeCallTopologySeatPorts(CallMediaSeat* seat) {
  CallTopologySeatPorts ports;
  if (!seat) {
    return ports;
  }
  ports.is_bound = [seat](const std::string& call_id) { return seat->IsBound(call_id); };
  ports.bound_call_id = [seat]() { return seat->BoundCallId(); };
  ports.acquire = [seat](const std::string& call_id) { return seat->Acquire(call_id); };
  ports.allows_path_op = [seat](const CallMediaSeat::Token& token) { return seat->AllowsPathOp(token); };
  ports.begin_attach = [seat](const std::string& call_id, const std::string& hop,
                              CallMediaSeat::AttachTicket* ticket) {
    return seat->BeginAttach(call_id, hop, ticket);
  };
  ports.end_attach_if_matching = [seat](const std::string& call_id, const std::string& hop) {
    seat->EndAttachIfMatching(call_id, hop);
  };
  ports.has_attach_in_flight = [seat]() { return seat->HasAttachInFlight(); };
  ports.attaching_hop = [seat]() { return seat->AttachingHopPeerId(); };
  ports.note_connecting = [seat](const std::string& call_id) { seat->NoteConnecting(call_id); };
  ports.note_start = [seat](const std::string& call_id) { seat->NoteStart(call_id); };
  ports.note_path = [seat](CallMediaSeat::PathKind kind) { seat->NotePath(kind); };
  ports.note_live = [seat](const std::string& call_id) { seat->NoteLive(call_id); };
  ports.cancel_attach_for_call = [seat](const std::string& call_id) { seat->CancelAttachForCall(call_id); };
  return ports;
}

} // namespace pbr
