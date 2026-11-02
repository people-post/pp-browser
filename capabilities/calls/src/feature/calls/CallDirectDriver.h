#pragma once

#include "common/media/CallMediaHealth.h"
#include "domain/call_media/ICallMediaTransport.h"
#include "feature/calls/CallDirectPathDeps.h"
#include "feature/calls/CallMediaDoorway.h"
#include "feature/calls/CallSessionEvents.h"
#include "lib/calls/CallMediaSeat.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** What the screen reads of a call's 1:1 path. */
struct CallDirectPathStatus {
  /** How the link runs (direct | punched | circuit …); empty before it does. */
  std::string media_path_kind;
  CallLinkCounters link;
  /** No 1:1 path could be kept for the call (it stays open, failed). */
  bool connect_failed = false;
  bool missing_mic = false;
  bool seed_unreachable = false;
  /** A connect attempt is dialing. */
  bool connect_inflight = false;
};

/**
 * One call's 1:1 direct media path (V056: built for that call by its media session; CallDirectPath
 * implements it). The session decides; the path runs. Calls owner.
 */
class CallDirectDriver {
public:
  virtual ~CallDirectDriver() = default;

  /** Arm and start the 1:1 connect for `call_id` (offerer reaches, answerer awaits). */
  virtual void ScheduleDirectStart(const std::string& call_id, const std::string& peer_identity, bool offerer) = 0;
  /** The call moved to the hop: drop the 1:1 transport under `token`, keep the engine running. */
  virtual void ReleaseDirectTransport(const CallMediaSeat::Token& token) = 0;
  /** Same, with no seat bound (harnesses). */
  virtual void ReleaseDirectTransport() = 0;
  /** Tear the 1:1 path down for `call_id` (engine included). */
  virtual void StopMeshMedia(const std::string& call_id) = 0;
  /** A failed, open call: Retry (this side dials again). */
  virtual Roe<void> RetryMeshMedia(const std::string& call_id) = 0;
  /** A failed, open call: resume over the peer's connection (PeerReconnected). */
  virtual Roe<void> ResumeMeshMediaFromInbound(const std::string& call_id) = 0;

  /** One of the path's own events, back on the calls owner. */
  virtual void Handle(DirectPathEvent& event) = 0;
  /** The call's media key landed (the deferred answerer goes on). */
  virtual void OnMediaKeyReady(const std::string& call_id) = 0;
  /** The doorway accepted an inbound bundle for the call: bind its peer, return its callbacks. */
  virtual CallMediaDirectCallbacks AcceptInbound(const CallMediaInboundHello& hello) = 0;
  /** H013: the call's peer gave up reaching us. */
  virtual void OnPeerGaveUpReaching(const std::string& call_id) = 0;
  /** The device's network changed. */
  virtual void OnLocalNetworkChanged() = 0;
  /** The call's path policy changed. */
  virtual void OnPathPolicyChanged(const std::string& call_id) = 0;
  /** A peer's mesh id → account learned from call control (stream identity). */
  virtual void NotePeerIdRelayMapping(const std::string& peer_id, const std::string& relay_identity) = 0;
  virtual CallDirectPathStatus Status() const = 0;
  /** The mesh is stopping: abort, cancel timers, refuse new work (the path is dropped after). */
  virtual void PrepareForTeardown(int timeout_ms) = 0;
  /** The mesh's reach goes away (mesh restart): let go of it until reattached. */
  virtual void DetachReach() = 0;
  /** The mesh is back on the same transport: take its reach and providers again. */
  virtual void AttachReach(const CallDirectPathDeps& deps) = 0;
};

} // namespace pbr
