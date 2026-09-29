#pragma once

#include "domain/messaging/CallLifecycleTypes.h"
#include "domain/messaging/CallMediaStatusLogic.h"
#include "feature/calls/CallMediaCoordinator.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** How this device came into the call. */
enum class LiveCallOrigin { Placed, Invited };

/**
 * Call-level state. How the media is doing (connecting / live / failed) is not a call state: a call
 * whose connection failed is still Joined — open until someone closes it.
 */
enum class LiveCallState {
  /** Placed; nobody has joined yet. */
  Calling,
  /** Invited; not answered. */
  Ringing,
  /** Accept in flight. */
  Accepting,
  /** In the call. */
  Joined,
  /** Closed (see LiveCallEndReason). */
  Ended,
};

/** Why a call ended on this device — set by the path that ended it, never inferred later. */
enum class LiveCallEndReason {
  None,
  /** This side left (hang up). */
  LocalLeave,
  /** The peer left or ended the call (1:1, or the last other participant of a group). */
  RemoteEnded,
  /** This side declined the invite. */
  Declined,
  /** The only invitee declined our call. */
  DeclinedByPeer,
  /** The invite expired unanswered here. */
  Expired,
  /** Our outbound call was never answered. */
  Unanswered,
  /** Accepting another call ended this one. */
  Superseded,
  /** No media path could be kept (e.g. the group hop never attached). */
  MediaUnavailable,
  /** The app is shutting down. */
  Shutdown,
  /** Left over from before a restart. */
  Orphaned,
};

/** How a call's media is doing (V037 Status) and what it has reached — per call. */
struct LiveCallMediaProgress {
  CallMediaStatus status = CallMediaStatus::None;
  /** Media went live since the last failure: the call shows InCall. */
  bool reached_live = false;
  /** The answerer waits for the call's media key (MediaPending). */
  bool key_pending = false;
};

const char* LiveCallStateName(LiveCallState state);
const char* LiveCallEndReasonName(LiveCallEndReason reason);

/**
 * One call as it lives on this device, from admission to close — the in-process counterpart of the
 * stored CallSession row. Persisted facts (epoch, key id, hints, participant rows) stay in the store;
 * this holds the runtime facts other parts used to keep their own copies of. Calls owner only.
 */
class LiveCall {
public:
  const std::string& Id() const { return call_id_; }
  /** Distinguishes a re-admitted call id; stale async work compares it. */
  uint64_t Instance() const { return instance_; }
  LiveCallOrigin Origin() const { return origin_; }
  LiveCallState State() const { return state_; }
  LiveCallEndReason EndReason() const { return end_reason_; }
  /** The state it was in when it closed (e.g. Ringing: a call this side never took). */
  LiveCallState StateAtClose() const { return state_at_close_; }
  /**
   * The peer ended (or declined) a call this side had placed or was in — news for the user.
   * Not a ring the caller withdrew or that expired.
   */
  bool EndedByPeer() const;
  bool IsOpen() const { return state_ != LiveCallState::Ended; }
  /** Remote roster identities in the call (account:…); a 1:1 call has one. */
  const std::vector<std::string>& Peers() const { return peers_; }
  std::optional<std::string> SolePeer() const;
  int64_t AdmittedAtMs() const { return admitted_at_ms_; }
  /** This call's media (engine + seat use); null until the call first needs media. */
  CallMediaCoordinator* Media() const { return media_.get(); }
  const LiveCallMediaProgress& MediaProgress() const { return progress_; }
  /**
   * What the call shows as (V037 State): Ringing / Accepting as its call state; a placed call nobody
   * answered yet is OutboundCalling; once in the call its media decides — ConnectFailed, InCall (live
   * since the last failure), MediaPending (key wait), MediaConnecting, else JoinedLocal. Ended: Idle.
   */
  CallPhase Phase() const;

private:
  friend class LiveCalls;

  std::string call_id_;
  uint64_t instance_ = 0;
  LiveCallOrigin origin_ = LiveCallOrigin::Placed;
  LiveCallState state_ = LiveCallState::Calling;
  LiveCallEndReason end_reason_ = LiveCallEndReason::None;
  LiveCallState state_at_close_ = LiveCallState::Ended;
  std::vector<std::string> peers_;
  int64_t admitted_at_ms_ = 0;
  std::unique_ptr<CallMediaCoordinator> media_;
  LiveCallMediaProgress progress_;
};

/**
 * The calls this device has: at most one Calling / Accepting / Joined (the active call) and any
 * number Ringing. Only these operations change a LiveCall. Ended calls stay findable (their end
 * reason) until the same id is admitted again or a newer ending pushes them out. Calls owner only.
 */
class LiveCalls {
public:
  LiveCall* Find(const std::string& call_id);
  const LiveCall* Find(const std::string& call_id) const;
  /** The one Calling / Accepting / Joined call, if any. */
  LiveCall* Active();
  const LiveCall* Active() const;
  std::vector<const LiveCall*> Ringing() const;
  /** The ring this device shows: the newest call still Ringing or being accepted; null if none. */
  const LiveCall* TheRing() const;
  /** The most recently closed call, if any is still kept. */
  const LiveCall* LastEnded() const;
  /** The call this device shows: the active call, else the ring; null when idle. */
  const LiveCall* Shown() const;
  /** The call being accepted, if any. */
  std::string AcceptingCallId() const;
  /** True when a call other than `call_id` is active (Calling / Accepting / Joined). */
  bool HasOtherActive(const std::string& call_id) const;

  // --- What the device shows (V037 State + Status), projected from the calls ---------------------
  /** The shown call's phase; Idle when none. */
  CallPhase Phase() const;
  /** A call's media Status (empty `call_id`: the active call); None when there is no such open call. */
  CallMediaStatus Status(const std::string& call_id = {}) const;
  /** The planner that Status arms. */
  CallArmedPlanner ArmedPlanner(const std::string& call_id = {}) const;
  /** The 1:1 path may start / run for the call. */
  bool AllowsDirectPath(const std::string& call_id = {}) const;
  /** The group path may attach / migrate / run for the call. */
  bool AllowsHopPath(const std::string& call_id = {}) const;
  /** A SoftMigrate may arm from the active call's Status. */
  bool SoftMigrateMayArm() const;
  /** Bumped when a call starts deciding its path or closes: late path work compares it and aborts. */
  uint64_t MediaCancelGen() const { return media_cancel_gen_; }
  /** Runs after anything that can change what the device shows (phase, Status, the shown call). */
  void SetOnChanged(std::function<void()> fn) { on_changed_ = std::move(fn); }

  // --- A call's media progress (empty `call_id`: the active call) -------------------------------
  void SetMediaStatus(const std::string& call_id, CallMediaStatus status, const char* reason);
  /** A path planner's progress, shown as the call's Status (CallMediaStatusLogic). */
  void ReportDirectProgress(const std::string& call_id, CallDirectPlannerPhase phase);
  void ReportHopProgress(const std::string& call_id, CallHopPlannerPhase phase);
  /** The 1:1 planner asks to arm while the Status does not allow it yet (set-up phases only). */
  void RequestDirectArming(const std::string& call_id);
  /** Our placed call starts deciding its path (unless a path already runs). */
  void NoteOutboundStarted(const std::string& call_id);
  /** The answerer's media waits for the key. */
  void NoteMediaDeferred(const std::string& call_id);
  /** The key arrived: the media connects. */
  void NoteMediaKeyReady(const std::string& call_id);
  /** The media connected (1:1 connect, or a live hop). */
  void NoteMediaConnected(const std::string& call_id);
  /** No media path: the call stays open, failed (Retry / the peer's reconnect resumes it). */
  void NoteMediaFailed(const std::string& call_id);

  /** This side placed the call. */
  LiveCall& AdmitPlaced(const std::string& call_id, const std::vector<std::string>& peers);
  /** An invite arrived. A redelivered invite for a call still open here changes nothing. */
  LiveCall& AdmitInvited(const std::string& call_id, const std::vector<std::string>& peers);
  /** Accept clicked (Ringing → Accepting). */
  void MarkAccepting(const std::string& call_id);
  /** The accept did not go through (Accepting → Ringing). */
  void MarkAcceptFailed(const std::string& call_id);
  /** This side is in the call: our accept landed, or a peer accepted our call. */
  void MarkJoined(const std::string& call_id);
  void AddPeer(const std::string& call_id, const std::string& identity);
  void RemovePeer(const std::string& call_id, const std::string& identity);
  /** Every end path. The first reason sticks; closing an unknown or ended call is a no-op. */
  void Close(const std::string& call_id, LiveCallEndReason reason);

  /** The one media engine and the media seat every call's coordinator drives (seat may be null). */
  void BindMediaResources(CallMediaEngine* engine, CallMediaSeat* seat);
  /** The 1:1 path the coordinators drive (null: none wired). */
  void BindDirectDriver(CallDirectDriver* direct) { resources_.direct = direct; }
  /** The group path the coordinators drive (null: harness). */
  void BindHopDriver(CallHopDriver* hop) { resources_.hop = hop; }
  /**
   * The call's media coordinator, created on first use. Null for a call this device never admitted
   * (or pruned), or before media resources are bound. An ended call keeps it for its stops.
   */
  CallMediaCoordinator* Media(const std::string& call_id);
  /**
   * Stop `call_id`'s media (empty: whatever holds the seat). The seat's release tears the paths
   * down; with no seat, each driver stops its part. Also for calls not admitted here (leftovers).
   */
  void StopMedia(const std::string& call_id);
  /**
   * Another call is about to take media: stop media still running for any call but `keep_call_id`
   * (an ended call can leave the engine running). Never touches `keep_call_id`'s own media.
   */
  void StopMediaExcept(const std::string& keep_call_id);
  /** The engine runs (1:1 or group capture). */
  bool MediaRunning() const;
  /** The call the engine runs for; empty when it runs for none (or is stopped). */
  std::string MediaRunningCallId() const;

private:
  LiveCall& Admit(const std::string& call_id, LiveCallOrigin origin, LiveCallState state,
                  const std::vector<std::string>& peers);
  /** A second open non-ringing call means a path skipped the one-active-call rule (step 3 enforces it). */
  void WarnIfSecondActive(const std::string& call_id) const;
  void PruneEnded();
  LiveCall* Resolve(const std::string& call_id);
  const LiveCall* Resolve(const std::string& call_id) const;
  void SetStatus(LiveCall& call, CallMediaStatus next, const char* reason);
  void Changed() const;

  std::map<std::string, LiveCall> calls_;
  uint64_t media_cancel_gen_ = 0;
  std::function<void()> on_changed_;
  CallMediaResources resources_;
  std::vector<std::string> ended_order_;
  uint64_t next_instance_ = 1;
};

} // namespace pbr
