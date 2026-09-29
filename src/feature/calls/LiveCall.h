#pragma once

#include <cstdint>
#include <map>
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
  /** The most recently closed call, if any is still kept. */
  const LiveCall* LastEnded() const;

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

private:
  LiveCall& Admit(const std::string& call_id, LiveCallOrigin origin, LiveCallState state,
                  const std::vector<std::string>& peers);
  /** A second open non-ringing call means a path skipped the one-active-call rule (step 3 enforces it). */
  void WarnIfSecondActive(const std::string& call_id) const;
  void PruneEnded();

  std::map<std::string, LiveCall> calls_;
  std::vector<std::string> ended_order_;
  uint64_t next_instance_ = 1;
};

} // namespace pbr
