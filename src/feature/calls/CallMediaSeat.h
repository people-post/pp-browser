#pragma once

#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V036 — exclusive process-wide bind between call_id and call media.
 * SoftMigrate = NotePath(Hop) under the same token (no Release).
 * Leave / leftover purge = Release (Detach then Stop).
 *
 * Phase 2: MediaState drives chrome (Connected only when Live). Attach flight
 * serializes SoftMigrate / CallSfuAttach under the seat (one hop at a time).
 * Phase 3: Direct/Hop path plugins take a seat Token (`AllowsPathOp` / `MatchesToken`).
 */
class CallMediaSeat : public Module {
public:
  enum class PathKind { None = 0, Direct, Hop };
  /** Dual-FSM media half — chrome Connected only when Live for the bound call. */
  enum class MediaState { Idle = 0, Connecting, Live, Failed };

  struct Token {
    uint64_t epoch = 0;
    std::string call_id;
  };

  struct AttachTicket {
    uint64_t gen = 0;
    std::string call_id;
    std::string hop_peer_id;
  };

  enum class AttachBeginResult {
    Started = 0,       // new flight; ticket filled
    CoalescedSameHop,  // same hop already attaching; ticket filled with in-flight gen
    DeferredOtherHop,  // different hop in flight — caller must defer
    Rejected,          // empty args / wrong call
  };

  /** Topology OnMediaStopped (Detach + clear attach state). */
  using TopologyStoppedFn = std::function<void(const std::string& call_id)>;
  /**
   * Engine Stop on UI (bridge StopMeshMedia body).
   * When force=false: no-op if Epoch() != epoch_at_post (NoteStart / Acquire advanced).
   * When force=true: always stop (Acquire releasing a prior bind).
   */
  using StopEngineFn =
      std::function<void(const std::string& call_id, uint64_t epoch_at_post, bool force)>;

  CallMediaSeat();

  void SetTeardownHooks(TopologyStoppedFn topology_stopped, StopEngineFn stop_engine);

  /** Exclusive bind. Releases any other bound call first. MediaState → Connecting. */
  Token Acquire(const std::string& call_id);
  /** Full teardown for call_id (or current bind if call_id empty and bound). → Idle. */
  void Release(const std::string& call_id);
  /** No-op when token.epoch != current Epoch(). */
  void Release(const Token& token);

  /**
   * Duplex StartSfu succeeded (or send-swap) for call_id — bump epoch so in-flight
   * Release cannot tear down the new session. Keeps Connecting until NoteLive.
   */
  void NoteStart(const std::string& call_id);
  void NotePath(PathKind kind);
  /** Explicit Connecting (pre-StartSfu dial / attach wait). No-op if already Live. */
  void NoteConnecting(const std::string& call_id);
  /** Duplex ready for chrome Connected (direct stream up or hop attach complete). */
  void NoteLive(const std::string& call_id);
  void NoteFailed(const std::string& call_id);

  /**
   * Single in-flight SFU attach under the seat. Same hop → coalesce; different hop →
   * defer. Does not bump media epoch.
   */
  AttachBeginResult BeginAttach(const std::string& call_id, const std::string& hop_peer_id,
                                AttachTicket* out);
  bool IsAttachCurrent(const AttachTicket& ticket) const;
  void EndAttach(const AttachTicket& ticket);
  /** Clear in-flight attach when call+hop still match (CompleteAttach success/fail). */
  void EndAttachIfMatching(const std::string& call_id, const std::string& hop_peer_id);
  void CancelAttachForCall(const std::string& call_id);
  bool HasAttachInFlight() const;
  std::string AttachingHopPeerId() const;

  bool IsBound(const std::string& call_id) const;
  bool IsLive(const std::string& call_id) const;
  /** Strict: epoch + call_id match CurrentToken (Release / stale Stop). */
  bool MatchesToken(const Token& token) const;
  /**
   * Path ops (Direct StartSfu / ReleaseTransport / Hop CompleteAttach) after NoteStart
   * bumps epoch — still valid while call_id remains bound.
   */
  bool AllowsPathOp(const Token& token) const;
  std::string BoundCallId() const;
  uint64_t Epoch() const;
  PathKind Path() const;
  MediaState State() const;
  Token CurrentToken() const;

private:
  void InvokeTeardown(const std::string& call_id, uint64_t epoch_at_post, bool force);
  void ClearAttachLocked();

  mutable std::mutex mu_;
  TopologyStoppedFn topology_stopped_;
  StopEngineFn stop_engine_;
  std::string bound_call_id_;
  uint64_t epoch_ = 0;
  PathKind path_ = PathKind::None;
  MediaState state_ = MediaState::Idle;
  uint64_t attach_gen_ = 0;
  std::string attach_call_id_;
  std::string attach_hop_peer_id_;
};

} // namespace pbr
