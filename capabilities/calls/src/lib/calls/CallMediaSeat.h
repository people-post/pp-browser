#pragma once

#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V036 — the exclusive process-wide lock between a call and the device's media: who holds it (the
 * bound call), its epoch (bumped by each start, so a Release posted earlier cannot stop a newer
 * session) and the one hop attach in flight (SoftMigrate / CallSfuAttach, one hop at a time).
 * Direct / Hop path plugins take a seat Token (`AllowsPathOp` / `MatchesToken`). Leave / leftover
 * purge = Release (Detach then Stop). A lock only (V055): how the call's media is doing and which
 * path carries it are the call's (`LiveCall`).
 */
class CallMediaSeat : public Module {
public:
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
   * Engine Stop on the calls owner (bridge StopMeshMedia body).
   * When force=false: no-op if Epoch() != epoch_at_post (NoteStart / Acquire advanced).
   * When force=true: always stop (Acquire releasing a prior bind).
   */
  using StopEngineFn =
      std::function<void(const std::string& call_id, uint64_t epoch_at_post, bool force)>;

  CallMediaSeat();

  void SetTeardownHooks(TopologyStoppedFn topology_stopped, StopEngineFn stop_engine);

  /** Exclusive bind. Releases any other bound call first. */
  Token Acquire(const std::string& call_id);
  /** Full teardown for call_id (or current bind if call_id empty and bound). */
  void Release(const std::string& call_id);
  /** No-op when token.epoch != current Epoch(). */
  void Release(const Token& token);

  /**
   * Duplex StartDuplex succeeded (or send-swap) for call_id — bump epoch so in-flight
   * Release cannot tear down the new session.
   */
  void NoteStart(const std::string& call_id);

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
  /** Strict: epoch + call_id match CurrentToken (Release / stale Stop). */
  bool MatchesToken(const Token& token) const;
  /**
   * Path ops (Direct StartDuplex / ReleaseTransport / Hop CompleteAttach) after NoteStart
   * bumps epoch — still valid while call_id remains bound.
   */
  bool AllowsPathOp(const Token& token) const;
  std::string BoundCallId() const;
  uint64_t Epoch() const;
  Token CurrentToken() const;

private:
  void InvokeTeardown(const std::string& call_id, uint64_t epoch_at_post, bool force);
  void ClearAttachLocked();

  mutable std::mutex mu_;
  TopologyStoppedFn topology_stopped_;
  StopEngineFn stop_engine_;
  std::string bound_call_id_;
  uint64_t epoch_ = 0;
  uint64_t attach_gen_ = 0;
  std::string attach_call_id_;
  std::string attach_hop_peer_id_;
};

} // namespace pbr
