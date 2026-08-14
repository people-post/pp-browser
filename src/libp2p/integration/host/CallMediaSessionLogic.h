#pragma once

#include "libp2p/integration/host/CallMediaDirectService.h"

namespace pbr {

/** Result of applying a call-media session event to the current phase (pure). */
enum class CallMediaSessionPhaseDecision {
  /** Transition to `next`. */
  Transition = 0,
  /** Event accepted but phase unchanged (e.g. dual-dial OpenStreamOk while HelloInbound). */
  Keep,
  /** Illegal / stale for this phase — ignore. */
  Ignore,
};

struct CallMediaSessionApplyContext {
  bool connect_waiter_active = false;
  bool has_stream = false;
};

struct CallMediaSessionPhaseOutcome {
  CallMediaSessionPhaseDecision decision = CallMediaSessionPhaseDecision::Ignore;
  CallMediaSessionPhase next = CallMediaSessionPhase::Idle;
};

/**
 * Pure call-media phase table (V033). Logging / SetPhase / stream IO stay in
 * CallMediaDirectService::Impl.
 */
CallMediaSessionPhaseOutcome DecideCallMediaSessionPhase(CallMediaSessionPhase phase,
                                                         CallMediaSessionEvent ev,
                                                         const CallMediaSessionApplyContext& ctx = {});

/**
 * Duplex Fail must not notify on_failed when already Detaching/Idle
 * (intentional Detach / SoftMigrate ReleaseDirect).
 */
bool CallMediaFailNotifySuppressed(CallMediaSessionPhase phase);

/**
 * Dual-dial / glare tie-break: the higher libp2p PeerId (base58) keeps outbound
 * and closes inbound; the lower PeerId yields and adopts inbound. Equal ids are
 * impossible across two hosts; treated as a loss so both sides cannot keep
 * independent outbound streams.
 */
bool LocalWinsCallMediaGlare(const std::string& local_peer_id, const std::string& remote_peer_id);

} // namespace pbr
