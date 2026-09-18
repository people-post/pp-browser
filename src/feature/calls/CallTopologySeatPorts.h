#pragma once

#include "feature/calls/CallMediaSeat.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Stack-filled MediaSeat façade for CallTopologyController (V046).
 * Fuller than CSM's CallMediaSeatPorts (release / bind_hop only).
 * Topology must not hold CallMediaSeat*.
 */
struct CallTopologySeatPorts {
  std::function<bool(const std::string& call_id)> is_bound;
  std::function<std::string()> bound_call_id;
  std::function<CallMediaSeat::Token(const std::string& call_id)> acquire;
  std::function<bool(const CallMediaSeat::Token& token)> allows_path_op;
  std::function<CallMediaSeat::AttachBeginResult(const std::string& call_id, const std::string& hop,
                                                 CallMediaSeat::AttachTicket* ticket)>
      begin_attach;
  std::function<void(const std::string& call_id, const std::string& hop)> end_attach_if_matching;
  std::function<bool()> has_attach_in_flight;
  std::function<std::string()> attaching_hop;
  std::function<void(const std::string& call_id)> note_connecting;
  std::function<void(const std::string& call_id)> note_start;
  std::function<void(CallMediaSeat::PathKind kind)> note_path;
  std::function<void(const std::string& call_id)> note_live;
  std::function<void(const std::string& call_id)> cancel_attach_for_call;

  bool IsBound() const { return static_cast<bool>(is_bound); }
};

/** Null seat → empty ports. */
CallTopologySeatPorts MakeCallTopologySeatPorts(CallMediaSeat* seat);

} // namespace pbr
