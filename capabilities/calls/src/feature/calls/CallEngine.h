#pragma once

#include "common/media_engine/MediaEngineHealth.h"
#include "common/media_engine/MediaTargets.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/media/MediaEngine.h"

#include <atomic>
#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * One call's handle on the one media engine (V057): what the call's paths may know and do with it.
 * Whether the engine runs this call, and on which path, is one fact: `CallMedia` (the only one that
 * starts or stops the engine) records the path it started; a stop from anywhere ends it. The paths
 * never read the engine's session id. The data plane writes (packets, targets, budgets, drops) reach
 * the engine only while it runs this call. Owned by the call's `CallMedia` and lent (shared) to its
 * paths; any thread (the 1:1 receive and the hop frames arrive on the transport's I/O).
 */
class CallEngine {
public:
  CallEngine(std::string call_id, MediaEngine& engine) : call_id_(std::move(call_id)), engine_(engine) {}
  CallEngine(const CallEngine&) = delete;
  CallEngine& operator=(const CallEngine&) = delete;

  const std::string& CallId() const { return call_id_; }

  // --- Who holds the media -------------------------------------------------------------------
  /** The engine runs this call's session. */
  bool Runs() const;
  /** The engine runs this call's session, started for `path`. */
  bool RunsOn(CallMediaPath path) const;
  /** The engine runs a session that is not this call's (another call's media, or a leftover). */
  bool RunsAnother() const;
  /** The session the engine runs, for logs only. */
  std::string RunningSessionForLog() const { return engine_.ActiveSessionId(); }

  // --- The data plane (only while the engine runs this call) ---------------------------------
  /** An inbound packet for this call's session; false (dropped) when the engine runs no session of this call. */
  bool Deliver(const MediaEngine::MediaPacket& packet);
  void NoteOutboundDrop();
  void NoteUplinkBudget(int64_t per_user_up_bps);
  void ApplyTargets(const MediaTargets& targets);

  // --- What the engine reports (the running session's) ---------------------------------------
  double PathPressure() const { return engine_.PathPressure(); }
  MediaEngineHealth HealthSnapshot() const { return engine_.HealthSnapshot(); }
  bool IsCameraEnabled() const { return engine_.IsCameraEnabled(); }
  bool IsMuted() const { return engine_.IsMuted(); }
  bool HasLocalCapture() const { return engine_.HasLocalCapture(); }
  int64_t StartedAtMs() const { return engine_.StartedAtMs(); }

private:
  friend class CallMedia;
  /** `CallMedia` started the engine for this call on `path`. */
  void NoteStarted(CallMediaPath path) { started_on_.store(static_cast<int>(path), std::memory_order_release); }
  MediaEngine& Engine() const { return engine_; }

  const std::string call_id_;
  MediaEngine& engine_;
  std::atomic<int> started_on_{static_cast<int>(CallMediaPath::Undecided)};
};

} // namespace pbr
