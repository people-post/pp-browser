#pragma once

#include "feature/calls/CallDevice.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallTypes.h"
#include "lib/pricing/PricingTypes.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/ThreadRecordTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * The calls on this device as the user and the peers ask for them (a2 / V014 / a4; p2p-av-calls V055,
 * PHASES § cm4): intents from the UI (`Apply`, start / accept / decline / leave, mic / camera) and
 * inbound call control (`ApplyInboundControl`: who may send what, then the workflow and the call's
 * media). It owns nothing: the parts are the device's (CallDevice), which it drives. Owned by
 * CallStack; calls owner only.
 */
class CallSessionManager : public Module {
public:
  explicit CallSessionManager(CallDevice& device);

  // --- What the user does (V037) ---------------------------------------------------------------
  /**
   * A UI event for `call_id` (empty: the ring for Accept / Decline, else the active call): a click runs
   * its session op. What the device shows is projected from the calls (LiveCalls::Phase / Status).
   */
  void Apply(CallLifecycleEvent ev, const std::string& call_id = {});
  Roe<CallSession> StartCall(const std::string& origin_thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);
  /** Accept a pending invite (the workflow's; own devices hear it was answered here). */
  Roe<void> AcceptInvite(const std::string& call_id,
                         InitiationChargeDecision charge_decision = InitiationChargeDecision::Waive);
  Roe<void> DeclineInvite(const std::string& call_id);
  Roe<void> LeaveCall(const std::string& call_id, LiveCallEndReason reason = LiveCallEndReason::LocalLeave);
  Roe<void> InviteParticipant(const std::string& call_id, const std::string& invitee_identity);
  /** Set before AcceptClicked — consumed by AcceptInvite. */
  void SetPendingAcceptChargeDecision(InitiationChargeDecision decision);
  /** Set before AcceptClicked — consumed (and reset to false) by AcceptInvite. */
  void SetPendingAcceptVoiceOnly(bool voice_only);
  /** Detach SFU + stop capture. Call before LeaveCall worker / app quit. */
  void StopCallMedia(const std::string& call_id);
  Roe<void> SetLocalAudioMuted(bool muted);
  /** `display_rotation_degrees` read on UI by the caller (L012). */
  Roe<void> SetLocalVideoEnabled(bool enabled, int display_rotation_degrees);
  /** Ask publisher for an IDR (empty identity = local encoder). */
  Roe<void> RequestVideoRefresh(const std::string& call_id, const std::string& publisher_identity);
  void SweepExpiredInvites();
  void AbandonOrphanedCallsAfterRestart();
  /** A stored Active call this process never had and nothing runs for: leave it (Orphaned). */
  void LeaveIfOrphaned(const std::string& call_id);
  // What the screen has shown (it takes each once).
  void ClearLastError() { device_.ClearLastError(); }
  void ClearMediaActivity() { device_.ClearMediaActivity(); }
  /** Clear only if still `seen` (the GUI showed it; a newer error stays). */
  void ClearLastMediaErrorIf(const std::string& seen) { device_.ClearLastMediaErrorIf(seen); }

  // --- What the peers send ---------------------------------------------------------------------
  Roe<void> ApplyInboundControl(ThreadMessage& message, const std::string& sender_identity,
                                std::optional<int64_t> relay_created_at_ms = std::nullopt,
                                std::optional<int64_t> relay_server_time_ms = std::nullopt);
  /** projects/privacy T3: who may call us; an invite from outside is dropped silently. */
  void SetInboundCallAudience(mesh::InboundAudience audience) { inbound_call_audience_ = audience; }
  /** Whether an invite from `caller` may ring (the audience; contacts from the disclosure gate). */
  bool AllowsInboundCall(const std::string& caller) const;

private:
  void ClickAccept(const std::string& call_id);
  void OnAcceptResult(const std::string& call_id, const Roe<void>& accepted);
  void ClickDecline(const std::string& call_id);
  void ClickLeave(const std::string& call_id);
  /** M021 d5: this device took / declined an incoming call — tell our other devices, so theirs stop ringing. */
  void TellOwnDevicesAnswered(const std::string& call_id, bool accepted);

  /** The control's call (its detail's call_id) has `sender_identity` as initiator or current participant. */
  bool SenderIsInCall(const std::string& detail_json, const std::string& sender_identity) const;
  /** V053: the call was offered to the sender (a group call): it may ask to join, or say it ended. */
  bool SenderMayJoin(const std::string& detail_json, const std::string& sender_identity) const;
  // Inbound call-control arms — thin delegates to the workflow and the call's media.
  Roe<void> HandleInboundInvite(const std::string& detail_json, const std::string& sender_identity,
                                const ThreadMessage& message, std::optional<int64_t> relay_created_at_ms,
                                std::optional<int64_t> relay_server_time_ms, const std::string& local_identity);
  Roe<void> HandleInboundAccept(const std::string& detail_json, const std::string& sender_identity,
                                const std::string& local_identity);
  Roe<void> HandleInboundDecline(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundLeave(const std::string& detail_json, const std::string& sender_identity,
                               const std::string& local_identity);
  Roe<void> HandleInboundRoster(const std::string& detail_json);
  /** A peer's CallMediaKey: its key taken, the workflow moves the call to its epoch. */
  Roe<void> HandleInboundMediaKey(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundSfuAttach(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundSfuAttachFailed(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundHopRefuse(const std::string& detail_json);
  Roe<void> HandleInboundVideoRefresh(const std::string& detail_json, const std::string& sender_identity);
  /** H013: our 1:1 caller stopped trying to reach us; the answerer stops waiting. */
  Roe<void> HandleInboundConnectGiveUp(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundEnded(const std::string& detail_json, const std::string& local_identity);
  /** M021 d5: a sibling's notice (our own account only) that it took / declined a call ringing here too. */
  Roe<void> HandleAnsweredElsewhere(const std::string& detail_json, const std::string& sender_identity,
                                    const std::string& local_identity);

  CallDevice& device_;
  // The device's parts this router drives (bound from it once).
  const ICallSessionReader& sessions_;
  LiveCalls& live_calls_;
  LiveCallMedia& call_media_;
  CallSessionWorkflow& workflow_;
  CallControlClient& control_;
  CallMediaKeyExchange& key_exchange_;
  CallReachSignals& reach_signals_;
  mesh::InboundAudience inbound_call_audience_ = mesh::InboundAudience::Everyone;
};

} // namespace pbr
