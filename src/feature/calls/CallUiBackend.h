#pragma once

#include "foundation/data/PricingTypes.h"
#include "common/media/CallMediaHealth.h"
#include "domain/messaging/CallTypes.h"
#include "common/Error.h"
#include "domain/messaging/CallLifecycleTypes.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallUiState.h"
#include "feature/calls/LiveCall.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaEngine;
class CallSessionManager;
class CallStack;

/**
 * Sealed UI-facing façade over the call stack (projects/thread-ownership t2b). Call state is owned
 * by the calls owner (`CallsThread`), never read or written from UI directly:
 * - **intents** post to the owner; ones with a result deliver it on UI through `on_done`;
 * - **owner state** (phase, status, flags, seat, hop health, …) reads the snapshot the owner
 *   publishes after each step (`CallStack::UiState`);
 * - **durable state** (invites, sessions, participants) reads the stores directly (read-only);
 * - `Media()` is the engine, which synchronizes itself (levels, video, health for the GUI).
 * Queries go through stack.Calls() per call so stack rebuilds stay transparent. Application owns
 * the instance (bound to ConversationsHub::CallStackRef()); CallController binds via
 * CallFunctionalPorts. UI thread.
 */
class CallUiBackend {
public:
  explicit CallUiBackend(CallStack& stack);

  bool Available() const;
  /** Stable pointer identity of the current CallSessionManager (stack rebuild detection). */
  const void* SessionsIdentity() const;

  /** GUI callbacks — always invoked on UI. */
  void SetOnRingChanged(std::function<void()> callback);
  void SetOnChromeRefresh(std::function<void()> callback);

  // --- Intents (posted to the calls owner) -----------------------------------------------------
  void SweepExpiredInvites();
  void PollP2pConnectHealth();
  void ClearMediaActivity();
  void Apply(CallLifecycleEvent ev, const std::string& call_id = {});
  void ClearLastError();
  void LeaveCall(const std::string& call_id, LiveCallEndReason reason = LiveCallEndReason::LocalLeave);
  void StopCallMedia(const std::string& call_id);
  void RequestVideoRefresh(const std::string& call_id, const std::string& publisher_identity);
  /** Set before AcceptClicked — consumed by AcceptInvite. */
  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);
  /** Set before AcceptClicked — consumed (and reset to false) by AcceptInvite. */
  void SetPendingAcceptVoiceOnly(bool voice_only);
  /** The pending media error, once per error (the owner clears it). */
  std::optional<std::string> TakeLastMediaError();
  /** A call the peer ended or declined while this side had it — once per call. */
  struct RemoteEnd {
    std::string call_id;
    bool declined = false;
  };
  std::optional<RemoteEnd> TakeRemoteEnd();

  // --- Intents with a result (`on_done` on UI) ---------------------------------------------------
  void StartCall(const std::string& origin_thread_id, bool video_allowed,
                 const std::vector<std::string>& invitee_identities, std::function<void(Roe<CallSession>)> on_done);
  void InviteParticipant(const std::string& call_id, const std::string& invitee_identity,
                         std::function<void(Roe<void>)> on_done);
  void SetLocalAudioMuted(bool muted, std::function<void(Roe<void>)> on_done = {});
  /** Reads the display rotation here, on UI (L012), and hands it to the owner. */
  void SetLocalVideoEnabled(bool enabled, std::function<void(Roe<void>)> on_done = {});

  // --- Durable state (stores, read-only) ---------------------------------------------------------
  Roe<std::optional<PendingCallInvite>> TopPendingInvite();
  Roe<std::optional<CallSession>> ActiveLocalCall();
  Roe<std::optional<std::string>> PeerIdentityForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> PeerVideoEnabledForCall(const std::string& call_id) const;
  Roe<std::optional<bool>> VideoAllowedForCall(const std::string& call_id) const;
  Roe<bool> AwaitingExplicitAnswerForCall(const std::string& call_id) const;
  Roe<std::vector<CallParticipant>> ListJoinedParticipants(const std::string& call_id) const;
  /** P001 initiation offer stored for inbound inviter (0 if none). */
  int64_t InitiationOfferMinorForPeer(const std::string& peer_identity) const;
  bool MediaAttemptedThisProcess(const std::string& call_id) const;

  // --- Owner state (published snapshot) ----------------------------------------------------------
  /** The whole snapshot, for readers that want one consistent view. */
  std::shared_ptr<const CallUiState> State() const;
  bool IsAwaitingSfuRecovery() const { return State()->awaiting_sfu_recovery; }
  bool IsSoftMigrateInFlight() const { return State()->soft_migrate_in_flight; }
  bool IsSfuAttachWaitActive() const { return State()->sfu_attach_wait_active; }
  bool IsP2pConnectFailed() const { return State()->p2p_connect_failed; }
  bool P2pConnectMissingMic() const { return State()->p2p_connect_missing_mic; }
  bool P2pConnectSeedUnreachable() const { return State()->p2p_connect_seed_unreachable; }
  std::string PeekMediaActivity() const { return State()->media_activity; }
  CallHopHealth HopHealth() const { return State()->hop_health; }
  std::string MediaPathKind() const { return State()->media_path_kind; }
  /** The call's link: the relay's when a hop is attached, else the 1:1 call-media link. */
  CallLinkCounters MediaLinkCounters() const {
    const auto state = State();
    return state->hop_health.attached ? state->hop_health.link : state->media_link;
  }
  CallMediaSeat::MediaState SeatMediaState(const std::string& call_id) const { return State()->SeatStateFor(call_id); }
  bool SeatMediaLive(const std::string& call_id) const { return State()->SeatLiveFor(call_id); }
  bool MediaChromeLive() const { return State()->MediaChromeLive(); }
  CallMediaStatus MediaStatus() const { return State()->media_status; }
  std::string LastError() const { return State()->last_error; }
  bool ShouldSuppressRing(const std::string& call_id) const { return State()->ShouldSuppressRing(call_id); }
  CallPhase Phase() const { return State()->phase; }
  std::string LastRingCallId() const { return State()->last_ring_call_id; }
  std::string ActiveCallId() const { return State()->active_call_id; }

  CallMediaEngine& Media();

private:
  /** Run `op` on the calls owner with the current session manager (skipped when there is none). */
  void OnOwner(std::function<void(CallSessionManager&)> op);
  template <typename R>
  static std::function<void(R)> ReplyOnUi(std::function<void(R)> on_done);

  CallStack& stack_;
  /** Last media error handed to the GUI (shown once until the owner clears it). */
  std::optional<std::string> taken_media_error_;
  std::string taken_remote_ended_;
};

} // namespace pbr
