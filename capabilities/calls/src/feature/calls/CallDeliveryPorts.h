#pragma once

#include "lib/delivery/SendRelayOptions.h"
#include "lib/delivery/ControlSend.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"
#include "lib/calls/CallGuests.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

/**
 * Delivery ops the calls (CallDevice) need from conversations (consumer-declared).
 * Softens calls→conversations: calls must not include MeshDeliveryOrchestrator.
 */
struct CallDeliveryPorts {
  /**
   * Call signaling to a peer on the control lane (no thread, never a transcript row; a copy per device of the
   * peer's, M024). `urgent`: a call invite — the relay wakes the peer as for an incoming call (P012).
   */
  std::function<pp::Roe<void>(const std::string& peer_identity, const ThreadMessage& control, bool urgent)>
      send_control;
  /**
   * Call signaling to many peers at once (V058 2): each resolved and sealed for its pair off the calls loop, in
   * parallel; `done` (optional, any thread) gets one result each.
   */
  std::function<void(std::vector<ControlSend> sends, ControlSendsDone done)> send_control_to_many;
  /**
   * A control notice to our own account's other devices (M021 d5: a call answered here stops ringing there);
   * nothing when this device is the account's only one. Unset: not told.
   */
  std::function<pp::Roe<void>(const ThreadMessage& control)> notify_own_devices;
  /** A message into a thread's chat stream (direct or group): the call summary. */
  std::function<pp::Roe<ThreadMessage>(const std::string& thread_id, const std::string& text,
                                       const SendRelayOptions& options)>
      send_to_thread;
  /** This device's DM with a peer: the private E2E one when there is one, else the public (made if missing). */
  std::function<pp::Roe<std::string>(const std::string& peer_identity)> resolve_peer_thread;
  /** This device's thread for a group; none when it has none. */
  std::function<std::optional<std::string>(const std::string& group_id)> resolve_group_thread;
  /** Who is in a group now (identities); none when this device does not know the group. */
  std::function<std::optional<std::set<std::string>>(const std::string& group_id)> group_members;
  /** V062: where an account stands in a group (none: this device holds no roster for it). Unset: not checked. */
  CallGroupStandingFn group_standing;
  std::function<void(bool force)> sync_inbox_from_wake;
  /**
   * The account pair's e2e_public session key, for the media key's old form (AutoKey if missing; M024 bridge — gone
   * with kMediaKeyAccountWrapBridge). An opener it made rides the lane's account copies to that peer until confirmed
   * (delivery's own doing).
   */
  std::function<pp::Roe<ByteVector>(const std::string& peer_identity)> ensure_peer_session_key;
  /**
   * The same key when a session with the peer is held — never opened: reading the media key's old form (a session
   * opened then could not open what the peer wrapped). Gone once the old form is no longer read.
   */
  std::function<pp::Roe<ByteVector>(const std::string& peer_identity)> held_peer_session_key;
  /**
   * After the last local call ends — flush inbox + TailSync that were deferred while
   * HasActiveLocalCall. No-op while another call is still ActiveLocalCall.
   */
  std::function<void()> catch_up_after_call;

  bool IsBound() const {
    return static_cast<bool>(send_control) && static_cast<bool>(send_control_to_many) &&
           static_cast<bool>(sync_inbox_from_wake);
  }
};

/**
 * A send to many made of `send_control`, one send after another on the caller's thread, `done` inline: for a
 * host without a control lane of its own (the call probe, tests).
 */
inline auto ControlSendsOneByOne(
    std::function<pp::Roe<void>(const std::string& peer_identity, const ThreadMessage& control, bool urgent)>
        send_control) {
  return [send_control = std::move(send_control)](std::vector<ControlSend> sends, ControlSendsDone done) {
    std::vector<ControlSendResult> results;
    results.reserve(sends.size());
    for (const ControlSend& send : sends) {
      results.push_back(ControlSendResult{.peer_identity = send.peer_identity,
                                          .queued = send_control(send.peer_identity, send.control, send.urgent)});
    }
    if (done) {
      done(std::move(results));
    }
  };
}

} // namespace pbr
