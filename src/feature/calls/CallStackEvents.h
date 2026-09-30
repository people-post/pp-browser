#pragma once

#include "common/Error.h"
#include "common/thread/ThreadRecordTypes.h"
#include "domain/messaging/CallMobility.h"
#include "feature/calls/CallSessionEvents.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class CallSessionManager;

/**
 * What the calls owner handles: inputs from other threads (edge adapters enqueue them) and the
 * delayed events its passive children asked for. Each is plain data in the vocabulary of whoever
 * reported it; CallStack routes it (THREADING.md § Owner runners).
 */
namespace calls_event {

/** k5 / k6: the device's attachment (`moved`: the local addresses changed). */
struct LocalNetworkChanged {
  MobilityAttachment attachment;
  bool changed = false;
  bool moved = false;
};
/** A NAT rebind: the mesh saw a new observed address. */
struct ObservedAddressChanged {};
/** Config `mesh.mobility` / `--mobility=` may have changed. */
struct MobilityOverrideChanged {};
/** The deadline CallPathMobility reported came. */
struct MobilityWake {};

/** Call control arrived (relay receive / Amp). */
struct CallControlReceived {
  ThreadMessage message;
  std::string sender_identity;
  std::optional<int64_t> relay_created_at_ms;
  std::optional<int64_t> relay_server_time_ms;
};

/** H011: the rendezvous relay (R1) our circuit reach chose. */
struct RelayChosen {
  std::string circuit_r1;
};
/** H012: the mesh asks for a punch signaled over call-control; `done` answers it (any thread). */
struct SignalingPunchRequested {
  std::string target_peer_id;
  std::vector<std::string> my_addrs;
  std::function<void(Roe<void>)> done;
};

/** The mesh learned an account's PeerId (dial-book registration, connectivity owner). */
struct MeshPeerIdLearned {
  std::string account_identity;
  std::string peer_id;
};
/** A command from the UI edge (`CallUiBackend`); `calls` is null without a session manager. */
struct SessionsCommand {
  std::function<void(CallSessionManager* calls)> run;
};

/** An event the session manager's subtree reported to itself; `generation` names that manager. */
struct ForSessions {
  uint64_t generation = 0;
  SessionEvent event;
};

} // namespace calls_event

using CallStackEvent =
    std::variant<calls_event::LocalNetworkChanged, calls_event::ObservedAddressChanged,
                 calls_event::MobilityOverrideChanged, calls_event::MobilityWake, calls_event::CallControlReceived,
                 calls_event::RelayChosen, calls_event::SignalingPunchRequested, calls_event::MeshPeerIdLearned,
                 calls_event::SessionsCommand, calls_event::ForSessions>;

/** For logs: the event's name. */
const char* CallStackEventName(const CallStackEvent& event);

} // namespace pbr
