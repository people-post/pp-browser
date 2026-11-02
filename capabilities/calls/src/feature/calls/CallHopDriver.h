#pragma once

#include "common/media/CallMediaHealth.h"
#include "feature/calls/CallSessionEvents.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallTypes.h"

#include "common/Error.h"

#include <cstddef>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * One call's group (hop / media_relay) media path (V056: built for that call by its media session;
 * CallHopPath implements it). The session asks; the path runs the hop. Calls owner.
 */
class CallHopDriver {
public:
  virtual ~CallHopDriver() = default;
  /** Our accept landed with `n_joined` in the call: true when the hop takes the call's media. */
  virtual bool OnLocalAcceptJoined(const std::string& call_id, size_t n_joined,
                                   const std::optional<std::string>& sfu_hint) = 0;
  /**
   * A peer accepted our call: true when the hop takes it (a group) — or the call is not the active
   * one; either way no 1:1 start.
   */
  virtual bool OnRemoteAcceptJoined(const std::string& call_id, size_t n_joined,
                                    const std::string& joiner_identity) = 0;
  /** Media runs on a hop now. */
  virtual bool IsSfuAttached() const = 0;
  /** A hop attach / migration / recovery is in flight. */
  virtual bool IsAwaitingSfuRecovery() const = 0;
  /** The call is (or is about to be) a group call on a hop: its 1:1 stream closing is expected. */
  virtual bool ExpectsGroupMedia(const std::string& call_id) const = 0;
  /** The 1:1 stream dropped ahead of the hop attach: wait for it (attach-wait). */
  virtual void BeginSfuAttachWait(const std::string& call_id) = 0;
  /** The call's media stopped: drop hop state for it — the no-seat stop path. */
  virtual void OnMediaStopped(const std::string& call_id) = 0;
  /** The camera was turned on / off: re-pick what the hop sends. */
  virtual void RefreshAdaptation(const std::string& call_id, bool camera_user_wants) = 0;

  /** One of the path's own events, back on the calls owner. */
  virtual void Handle(HopPathEvent& event) = 0;
  /** The call's media key epoch landed (the hop re-keys its streams). */
  virtual void OnMediaKeyInstalled(const std::string& call_id) = 0;
  virtual void OnJoinedCountObserved(const std::string& call_id, size_t n_joined) = 0;
  virtual void ClearSfuAttachWait() = 0;
  /** Inbound call control for the call's hop (V025 / V029 / V050). */
  virtual Roe<void> OnInboundSfuAttach(const std::string& call_id, const CallSfuAttachDetail& attach,
                                       const std::string& sender) = 0;
  virtual void OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail& detail) = 0;
  virtual void OnInboundHopRefuse(const CallHopRefuseDetail& detail) = 0;
  /** The engine runs the call on the hop. */
  virtual bool IsOnSfuForCall(const std::string& call_id) const = 0;
  /** Why the hop path is being recovered (None: it is not). */
  virtual CallRecovery Recovery() const = 0;
  virtual CallHopHealth HopHealth() const = 0;
  virtual void OnPeerMediaRelayCapLearned(const std::string& call_id, const std::string& peer_id) = 0;
  virtual void OnLocalNetworkMoved() = 0;
  /** The device's relay deps are about to change (the relay client is still alive) / changed. */
  virtual void UnwatchRelay() = 0;
  virtual void OnRelayDepsChanged() = 0;
};

} // namespace pbr
