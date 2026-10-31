#include "feature/calls/CallPresenter.h"

#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallTypes.h"
#include "lib/media/CallAudioSession.h"
#include "lib/media/CallMediaEngine.h"
#include "feature/calls/CallUiBackend.h"
#include "foundation/data/PricingTypes.h"
#include "common/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** Remote frames this fresh mean the peer's camera is on, whatever its roster row says (B58). */
constexpr int64_t kPeerVideoLiveMs = 500;

} // namespace

CallPresenter::CallPresenter() {
  redirectLogger("CallPresenter");
}

void CallPresenter::BindPorts(CallFunctionalPorts ports) {
  ports_ = std::move(ports);
  BindToMessaging();
}

void CallPresenter::BindCallbacks(Callbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

bool CallPresenter::MessagingInitialized() const {
  return ports_.initialized && ports_.initialized();
}

CallUiBackend* CallPresenter::Backend() const {
  return ports_.backend ? ports_.backend() : nullptr;
}

void CallPresenter::BindToMessaging() {
  if (!MessagingInitialized()) {
    bound_calls_ = nullptr;
    return;
  }
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    bound_calls_ = nullptr;
    return;
  }
  // CallSessionManager is recreated in BuildMessagingStack; rebind when the pointer changes.
  const void* identity = backend->SessionsIdentity();
  if (bound_calls_ == identity) {
    return;
  }
  // Ingest may run on IO; the screen is decided on UI.
  backend->SetOnRingChanged([this]() { AppRuntime::PostUI([this]() { Refresh(); }); });
  backend->SetOnChromeRefresh([this]() { Refresh(); });
  bound_calls_ = identity;
  // Pick up post-restart abandon / pending ring after stack rebuild.
  Refresh();
}

void CallPresenter::Tick() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend) {
    return;
  }
  backend->SweepExpiredInvites();
  if (!active_call_id_.empty() && backend->Available()) {
    Refresh();  // Calling… / the timer / levels stay fresh, before media starts too
  }
}

void CallPresenter::OnCallWake() {
  // Inbox sync is async: the ring (and its notification) comes once Refresh sees the invite.
  Refresh();
}

void CallPresenter::Notify(CallNotice notice) {
  if (callbacks_.notify) {
    callbacks_.notify(notice);
  }
}

void CallPresenter::Fail(const std::string& key, const StartDone& done) {
  CallNotice notice;
  notice.kind = CallNotice::Kind::Error;
  notice.key = key;
  Notify(notice);
  if (done) {
    CallStartResult result;
    result.error_key = key;
    done(result);
  }
}

void CallPresenter::FailText(const std::string& text) {
  CallNotice notice;
  notice.kind = CallNotice::Kind::Error;
  notice.text = text;
  Notify(notice);
}

void CallPresenter::Changed() {
  if (callbacks_.on_changed) {
    callbacks_.on_changed();
  }
}

bool CallPresenter::TakeStackNotices(CallUiBackend& backend) {
  if (!backend.LastError().empty()) {
    FailText(backend.LastError());
    backend.ClearLastError();
  }
  if (auto media_err = backend.TakeLastMediaError(); media_err && !media_err->empty()) {
    // SoftMigrate / hop failures: a toast vanishes before multi-hop diagnostics can be read.
    CallNotice notice;
    notice.kind = CallNotice::Kind::StickyError;
    notice.text = *media_err;
    Notify(notice);
  }
  // The peer ended a call we were in (also a failed one — failed is not closed): the panel is
  // about to go, so say why instead of letting the call silently vanish.
  if (auto ended = backend.TakeRemoteEnd(); ended) {
    CallNotice notice;
    notice.kind = ended->declined ? CallNotice::Kind::PeerDeclined : CallNotice::Kind::PeerEnded;
    if (auto peer = backend.PeerIdentityForCall(ended->call_id); peer && peer->has_value()) {
      notice.identity = **peer;
    }
    Notify(notice);
    return true;
  }
  return false;
}

void CallPresenter::Refresh() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { Refresh(); });
    return;
  }
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }
  const bool remote_ended = TakeStackNotices(*backend);

  // Attach-wait / connect health are SM-owned timers (V039 pm3); the state steps a refresh owes
  // the owner (invite seen / cleared, orphan leave) happen in Settle.
  const CallUiBackend::Surface surface = backend->Settle();
  using SurfaceKind = CallUiBackend::Surface::Kind;
  CallScreen next;
  // A click the stack has not taken yet: its ring stays answered / declined meanwhile.
  const std::string ringing_id = surface.kind == SurfaceKind::Ringing ? surface.invite->call_id : std::string();
  if (answered_call_id_ != ringing_id) {
    answered_call_id_.clear();
  }
  if (declined_call_id_ != ringing_id) {
    declined_call_id_.clear();
  }

  if (surface.kind == SurfaceKind::Ringing && !declined_call_id_.empty()) {
    // Declined: nothing shows (the call this device is in, if any, stays active).
    if (surface.active) {
      active_call_id_ = surface.active->call_id;
    }
  } else if (surface.kind == SurfaceKind::Answering ||
             (surface.kind == SurfaceKind::Ringing && !answered_call_id_.empty())) {
    // Accept in flight (CALLS.md): the ring is gone; a Connecting bar so Accept does not look hung.
    const PendingCallInvite& top = *surface.invite;
    ringing_call_id_.clear();
    ring_shown_ = false;
    next.kind = CallScreen::Kind::Answering;
    next.ring_call_id = top.call_id;
    next.caller_identity = top.inviter_identity;
    next.ring_video = top.video_allowed;
    next.answered_video = auto_camera_pending_ && auto_camera_call_id_ == top.call_id;
  } else if (surface.kind == SurfaceKind::Ringing) {
    const PendingCallInvite& top = *surface.invite;
    ringing_call_id_ = top.call_id;
    next.kind = CallScreen::Kind::Ringing;
    next.ring_call_id = top.call_id;
    next.caller_identity = top.inviter_identity;
    next.ring_video = top.video_allowed;
    if (surface.active) {
      // Ringing over the call we are in: answering ends that one. Keep it as the active call.
      next.conflict = true;
      active_call_id_ = surface.active->call_id;
      if (auto peer = backend->PeerIdentityForCall(surface.active->call_id); peer && peer->has_value()) {
        next.conflict_peer_identity = **peer;
        next.conflict_same_peer = **peer == top.inviter_identity;
      }
    }
    // P001: waive / take-all when the inviter offered a positive initiation amount.
    next.offer_minor = backend->InitiationOfferMinorForPeer(top.inviter_identity);
    next.charge_enabled = next.offer_minor > 0 && PaymentRailsAvailable();
    if (!ring_shown_) {
      ring_shown_ = true;
      log().warning << "ring shown call_id=" << top.call_id;
      if (callbacks_.on_ring_started) {
        callbacks_.on_ring_started(top.call_id, top.video_allowed);
      }
      // Every ring announces itself, however the invite arrived (a push, a sync, the mesh): the
      // platform's notifier shows it only when the user is not looking at the app (in the
      // background, a minimized window).
      CallNotice notice;
      notice.kind = CallNotice::Kind::IncomingCall;
      notice.identity = top.inviter_identity;
      Notify(notice);
    }
  } else if (surface.kind == SurfaceKind::InCall) {
    active_call_id_ = surface.active->call_id;
    ringing_call_id_.clear();
    ring_shown_ = false;
    FillInCall(next, *backend, surface.active->call_id, surface.active->video_allowed,
               surface.active->origin_group_id.has_value());
    MaybeAutoCamera(*backend, next);
  } else {
    active_call_id_.clear();
    ringing_call_id_.clear();
    ring_shown_ = false;
  }
  const int64_t now = util::NowUnixMs();
  // The screen's call went away by itself only when nothing replaced it and the stack is not still in it
  // (a ring over it, a declined ring, a merged call keep or replace it).
  const bool went_away = next.kind == CallScreen::Kind::None &&
                         (!surface.active || surface.active->call_id != screen_.call_id);
  if (went_away && EndedWithoutConnecting(screen_, next, remote_ended)) {
    CallNotice notice;
    notice.kind = CallNotice::Kind::Error;
    notice.key = "call.status.ended_couldnt_connect";
    Notify(notice);
  } else if (went_away && EndedAlone(screen_, next, now)) {
    CallNotice notice;
    notice.kind = CallNotice::Kind::Error;
    notice.key = "call.status.ended_alone";
    Notify(notice);
  }
  WatchForNoMedia(*backend, next, now);
  screen_ = std::move(next);
  Changed();
}

void CallPresenter::FillInCall(CallScreen& s, CallUiBackend& backend, const std::string& call_id, const bool video,
                               const bool group_call) {
  s = {};
  s.kind = CallScreen::Kind::InCall;
  s.call_id = call_id;
  s.video = video;
  s.muted = backend.Media().IsMuted();
  s.show_speaker = CallAudioSession::SupportsSpeakerToggle();
  s.speaker_on = CallAudioSession::IsSpeakerphoneOn();
  std::string local_identity;
  if (ports_.local_relay_identity) {
    local_identity = ports_.local_relay_identity().value_or(std::string{});
  }
  int joined = 0;
  if (auto participants = backend.ListJoinedParticipants(call_id); participants) {
    joined = static_cast<int>(participants->size());
    s.group = joined > 2 || group_call;
    s.members.reserve(participants->size());
    for (const CallParticipant& row : *participants) {
      CallScreen::Member member;
      member.identity = row.identity;
      member.is_local = !local_identity.empty() && row.identity == local_identity;
      member.audio_muted = row.media.audio_muted;
      member.video_enabled = row.media.video_enabled;
      s.members.push_back(std::move(member));
    }
  }
  if (auto peer = backend.PeerIdentityForCall(call_id); peer && peer->has_value()) {
    s.peer_identity = **peer;
  }

  InCallSignals signals;
  signals.p2p_connect_failed = backend.IsP2pConnectFailed();
  signals.p2p_missing_mic = backend.P2pConnectMissingMic();
  signals.p2p_seed_unreachable = backend.P2pConnectSeedUnreachable();
  // The hop path being recovered, also a guest's lost transport between reattach attempts (the
  // Reconnecting line stays up; the seat stays Live by design).
  signals.recovery = backend.Recovery();
  signals.sfu_mode = backend.Media().IsSfuMode();
  signals.group_context =
      group_call || joined > 2 || signals.recovery != CallRecovery::None || signals.sfu_mode;
  signals.media_chrome_live = backend.MediaChromeLive();
  signals.media_status = backend.MediaStatus();
  signals.media_active = backend.Media().IsActive();
  signals.connection_state = backend.Media().ConnectionState();
  signals.activity = backend.PeekMediaActivity();
  const int64_t live_since_ms = backend.LiveSinceMs(call_id);
  signals.was_connected = live_since_ms > 0;
  const InCallStatus decided = DecideInCallStatus(signals);
  if (decided.activity_spent) {
    backend.ClearMediaActivity();
  }
  s.status = decided.status;
  s.media_connected = decided.media_connected;
  s.was_connected = signals.was_connected;
  s.show_retry = decided.show_retry;
  s.show_invite = decided.show_invite;
  s.hint = decided.hint;
  s.hint_missing_mic = signals.p2p_missing_mic;
  s.hint_seed_unreachable = signals.p2p_seed_unreachable;
  if (decided.status == CallScreen::Status::Activity) {
    s.activity = signals.activity;
  } else if (decided.status == CallScreen::Status::Raw) {
    s.raw_state = signals.connection_state;
  }
  if (const int64_t alone_until = backend.AloneUntilMs(); s.group && alone_until > 0) {
    // V053: everyone else left; the line counts down to this device leaving by itself.
    s.status = CallScreen::Status::WaitingForOthers;
    s.alone_until_ms = alone_until;
  }
  if (decided.show_elapsed || ShowsParticipantCount(s)) {
    s.connected_at_ms = live_since_ms;  // the call's, not the current path's
  }
}

void CallPresenter::WatchForNoMedia(CallUiBackend& backend, const CallScreen& s, const int64_t now) {
  // Once a second is plenty (the health snapshot takes the engine's lock), and only a group call counts.
  if (now - no_media_checked_ms_ < 1'000) {
    return;
  }
  no_media_checked_ms_ = now;
  NoMediaSample sample;
  if (s.kind == CallScreen::Kind::InCall && s.group) {
    const CallMediaEngineHealth health = backend.Media().HealthSnapshot();
    sample.call_id = s.call_id;
    sample.group = s.group;
    // A hop reattach keeps the seat "live" while nothing flows: recovering pauses the count too.
    sample.media_live = s.media_connected && backend.Recovery() == CallRecovery::None &&
                        s.status != CallScreen::Status::Reconnecting;
    sample.others_joined = static_cast<size_t>(
        std::count_if(s.members.begin(), s.members.end(), [](const CallScreen::Member& m) { return !m.is_local; }));
    sample.last_rx_ms = std::max(health.last_rx_audio_ms, health.last_rx_video_ms);
    sample.now_ms = now;
  }
  if (!StepNoMediaWatch(no_media_watch_, sample)) {
    return;
  }
  // B97: live, others in the call, and nothing from any of them for 30 s — the call is gone in all but name.
  log().warning << "Nothing heard from anyone in the group call for " << kGroupNoMediaEndMs / 1000
                << " s — leaving call_id=" << s.call_id;
  no_media_watch_ = {};
  CallNotice notice;
  notice.kind = CallNotice::Kind::Error;
  notice.key = "call.status.ended_no_media";
  Notify(notice);
  // Not from inside this refresh: leaving refreshes the screen again.
  AppRuntime::PostUI(WhileAlive([this, call_id = s.call_id]() {
    if (active_call_id_ == call_id) {
      (void)Leave();
    }
  }));
}

void CallPresenter::MaybeAutoCamera(CallUiBackend& backend, const CallScreen& s) {
  // 2026-09-28: video calls start with the local camera on, exactly once, and only when the call
  // is connected (never before the callee answered). It stays pending while the camera button would
  // be hidden — no encoder, or the path disallows video — and on the caller while the answer is
  // only implicit (B30), so a voice answer still in flight is not sent our video first.
  if (!auto_camera_pending_ || auto_camera_call_id_ != active_call_id_ || !s.media_connected) {
    return;
  }
  const bool camera_offerable = backend.Media().VideoEncoderAvailable() && backend.Media().CameraPathAllowsVideo();
  auto awaiting_answer = backend.AwaitingExplicitAnswerForCall(active_call_id_);
  // In a 1:1 call the peer's video arriving also answers "video or voice?" (a slow relay delivered
  // the explicit Accept 51 s after connect, 2026-09-30). Not in a group: one member's video says
  // nothing about another invitee whose answer is still in flight.
  const bool peer_video_answers = !s.group && backend.Media().IsRemoteVideoLive(kPeerVideoLiveMs);
  const bool answer_known = (awaiting_answer && !*awaiting_answer) || peer_video_answers;
  auto allowed = backend.VideoAllowedForCall(active_call_id_);
  if (allowed && allowed->has_value() && **allowed && camera_offerable && answer_known) {
    auto_camera_pending_ = false;
    if (!backend.Media().IsCameraEnabled()) {
      log().info << "auto camera on call_id=" << active_call_id_;
      backend.SetLocalVideoEnabled(true, WhileAlive([call_id = active_call_id_](Roe<void> cam) {
        if (!cam) {
          logging::getLogger("CallPresenter").warning << "auto camera on failed call_id=" << call_id << ": "
                                                      << cam.error().message;
        }
      }));
    }
  } else if (allowed && allowed->has_value() && !**allowed) {
    // Narrowed to voice-only before we got here: drop the intent (the renderer's camera-off sync
    // turns the camera off for this case).
    auto_camera_pending_ = false;
  }
}

// --- buttons ------------------------------------------------------------------------------------

bool CallPresenter::StartCall(const std::string& thread_id, const bool video, StartDone done) {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    Fail("call.error.unavailable", done);
    return false;
  }
  auto thread = ports_.get_thread ? ports_.get_thread(thread_id)
                                  : Roe<std::optional<Thread>>::error(Error("call port unavailable"));
  if (!thread || !*thread) {
    Fail("call.error.thread_not_found", done);
    return false;
  }
  if ((*thread)->kind == ThreadKind::Group) {
    if (backend->JoinableCallInThread(thread_id)) {
      RequestStart(thread_id, false, {}, std::move(done));  // V053: the group's call goes on — join it
      return true;
    }
    if (callbacks_.open_group_picker) {
      callbacks_.open_group_picker(thread_id, video);
    }
    if (done) {
      CallStartResult result;
      result.kind = CallStartResult::Kind::PickPeople;
      done(result);
    }
    return true;
  }
  if ((*thread)->kind != ThreadKind::Direct) {
    Fail("call.error.wrong_thread_type", done);
    return false;
  }
  if ((*thread)->peer_identity_value.empty()) {
    Fail("call.error.missing_peer", done);
    return false;
  }
  return StartCallWithInvitees(thread_id, video, {(*thread)->peer_identity_value}, std::move(done));
}

bool CallPresenter::StartCallWithInvitees(const std::string& thread_id, const bool video,
                                          const std::vector<std::string>& invitees, StartDone done) {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    Fail("call.error.unavailable", done);
    return false;
  }
  if (invitees.empty()) {
    Fail("call.error.select_person", done);
    return false;
  }
  RequestStart(thread_id, video, invitees, std::move(done));
  return true;
}

void CallPresenter::RequestStart(const std::string& thread_id, const bool video,
                                 const std::vector<std::string>& invitees, StartDone done) {
  // No invitees: joining the group's call that goes on (not an outbound call).
  const bool joining = invitees.empty();
  // The calls owner starts it and reports back on UI.
  Backend()->StartCall(thread_id, video, invitees,
                       WhileAlive([this, video, joining, done = std::move(done)](Roe<CallSession> started) {
    CallStartResult result;
    if (!started) {
      FailText(started.error().message);
      result.error_text = started.error().message;
      if (done) {
        done(result);
      }
      return;
    }
    active_call_id_ = started->call_id;
    result.kind = joining ? CallStartResult::Kind::Joined : CallStartResult::Kind::Started;
    result.call_id = started->call_id;
    if (!joining) {
      if (callbacks_.on_outbound_started) {
        callbacks_.on_outbound_started(started->call_id, video);
      }
      if (video) {
        // The caller picked "Video call": the camera comes on once the call connects.
        auto_camera_pending_ = true;
        auto_camera_call_id_ = started->call_id;
      }
    }
    if (done) {
      done(result);
    }
    Refresh();
  }));
}

bool CallPresenter::CallJoinableInThread(const std::string& thread_id) const {
  auto* backend = Backend();
  return backend && backend->Available() && backend->JoinableCallInThread(thread_id).has_value();
}

void CallPresenter::OpenAddPeople() {
  BindToMessaging();
  auto* backend = Backend();
  if (backend && backend->Available() && active_call_id_.empty()) {
    if (auto active = backend->ActiveLocalCall(); active && active->has_value()) {
      active_call_id_ = (*active)->call_id;
    }
  }
  if (active_call_id_.empty()) {
    Fail("call.error.no_active");
    return;
  }
  if (callbacks_.open_add_people) {
    callbacks_.open_add_people(active_call_id_);
  }
}

void CallPresenter::InviteToActiveCall(const std::vector<std::string>& identities,
                                       std::function<void(Roe<void>)> done) {
  BindToMessaging();
  auto* backend = Backend();
  if (backend && backend->Available() && active_call_id_.empty()) {
    if (auto active = backend->ActiveLocalCall(); active && active->has_value()) {
      active_call_id_ = (*active)->call_id;
    }
  }
  if (!backend || !backend->Available() || active_call_id_.empty()) {
    Fail("call.error.no_active");
    if (done) {
      done(Error("no call in progress"));
    }
    return;
  }
  for (const std::string& identity : identities) {
    if (identity.empty()) {
      continue;
    }
    backend->InviteParticipant(active_call_id_, identity, WhileAlive([this, done](Roe<void> ok) {
      if (!ok) {
        FailText(ok.error().message);
      } else {
        Refresh();
      }
      if (done) {
        done(ok);
      }
    }));
  }
}

bool CallPresenter::VideoCallAvailable() {
  BindToMessaging();
  auto* backend = Backend();
  return backend && backend->Available() && backend->Media().VideoEncoderAvailable();
}

std::string CallPresenter::RingCallId(CallUiBackend& backend) const {
  if (!ringing_call_id_.empty()) {
    return ringing_call_id_;
  }
  if (!screen_.ring_call_id.empty()) {
    return screen_.ring_call_id;
  }
  return backend.LastRingCallId();
}

std::string CallPresenter::Accept(const bool voice_only) {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return {};
  }
  const std::string call_id = RingCallId(*backend);
  if (call_id.empty()) {
    log().warning << "Accept ignored (no call_id)";
    return {};
  }
  backend->SetPendingAcceptChargeDecision(InitiationChargeDecision::Waive);
  backend->SetPendingAcceptVoiceOnly(voice_only);
  if (callbacks_.on_accept) {
    callbacks_.on_accept(call_id, voice_only);
  }
  if (!voice_only && screen_.ring_video) {
    // Answered a video call with "Video": the camera comes on once connected, as on the caller.
    auto_camera_pending_ = true;
    auto_camera_call_id_ = call_id;
  } else if (auto_camera_call_id_ == call_id) {
    auto_camera_pending_ = false;  // answered voice-only: never the camera for this call
  }
  // The ring goes on the click frame (CALLS.md Accept → Accepting): leaving it up until
  // AcceptInvite finished made Accept look hung.
  ringing_call_id_.clear();
  ring_shown_ = false;
  answered_call_id_ = call_id;
  screen_ = {};
  Changed();
  log().warning << "Accept → lifecycle AcceptClicked call_id=" << call_id << " charge=waive"
                << (voice_only ? " voice_only=1" : "");
  backend->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  return call_id;
}

std::string CallPresenter::AcceptWithCharge() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return {};
  }
  if (!PaymentRailsAvailable()) {
    Fail("call.ring.accept_charge_disabled_hint");
    return {};
  }
  const std::string call_id = RingCallId(*backend);
  if (call_id.empty()) {
    log().warning << "AcceptWithCharge ignored (no call_id)";
    return {};
  }
  backend->SetPendingAcceptChargeDecision(InitiationChargeDecision::TakeAll);
  if (callbacks_.on_accept) {
    callbacks_.on_accept(call_id, false);
  }
  backend->SetPendingAcceptVoiceOnly(false);
  if (screen_.ring_video) {
    // A charge-accept of a video call is still a "Video" accept.
    auto_camera_pending_ = true;
    auto_camera_call_id_ = call_id;
  }
  ringing_call_id_.clear();
  ring_shown_ = false;
  answered_call_id_ = call_id;
  screen_ = {};
  Changed();
  log().warning << "AcceptWithCharge → lifecycle AcceptClicked call_id=" << call_id << " charge=take_all";
  backend->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  return call_id;
}

std::string CallPresenter::Decline() {
  BindToMessaging();
  auto* backend = Backend();
  std::string call_id = ringing_call_id_;
  if (call_id.empty() && backend && backend->Available()) {
    call_id = backend->LastRingCallId();
  }
  if (call_id.empty()) {
    return {};
  }
  ringing_call_id_.clear();
  ring_shown_ = false;
  declined_call_id_ = call_id;
  screen_ = {};
  Changed();
  if (callbacks_.on_decline) {
    callbacks_.on_decline(call_id);
  }
  if (backend && backend->Available()) {
    backend->Apply(CallLifecycleEvent::DeclineClicked, call_id);
  }
  return call_id;
}

std::string CallPresenter::Leave() {
  BindToMessaging();
  auto* backend = Backend();
  std::string call_id = active_call_id_;
  if (call_id.empty() && backend && backend->Available()) {
    call_id = backend->ActiveCallId();
  }
  if (!call_id.empty() && backend && backend->Available()) {
    // Detach the hop then stop the devices on UI before the LeaveCall worker (they must not stop
    // off-UI), and before the End button goes. Media().Stop alone deadlocks if capture is in
    // BlockingWrite.
    backend->StopCallMedia(call_id);
  }
  // No call: a stale End button after Idle — the screen still clears, so nothing looks hung.
  active_call_id_.clear();
  ringing_call_id_.clear();
  ring_shown_ = false;
  screen_ = {};
  Changed();
  if (!call_id.empty() && backend && backend->Available()) {
    backend->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  }
  return call_id;
}

void CallPresenter::Retry() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }
  const std::string call_id = active_call_id_.empty() ? backend->ActiveCallId() : active_call_id_;
  if (call_id.empty()) {
    return;
  }
  backend->Apply(CallLifecycleEvent::RetryClicked, call_id);
  Refresh();  // a refusal reads as the stack's last error, which Refresh reports
}

void CallPresenter::ToggleMute() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available() || !backend->Media().IsActive()) {
    return;
  }
  const bool before = backend->Media().IsMuted();
  backend->SetLocalAudioMuted(!before, WhileAlive([this](Roe<void> muted) {
    if (!muted) {
      FailText(muted.error().message);
    }
    Refresh();
  }));
}

void CallPresenter::ToggleCamera() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available() || !backend->Media().IsActive()) {
    return;
  }
  if (active_call_id_.empty()) {
    if (auto active = backend->ActiveLocalCall(); active && active->has_value()) {
      active_call_id_ = (*active)->call_id;
    }
  }
  const bool next = !backend->Media().IsCameraEnabled();
  if (next && !active_call_id_.empty()) {
    if (auto allowed = backend->VideoAllowedForCall(active_call_id_);
        !allowed || !allowed->has_value() || !**allowed) {
      Fail("call.error.video_not_allowed");
      return;
    }
  }
  backend->SetLocalVideoEnabled(next, WhileAlive([this](Roe<void> cam) {
    if (!cam) {
      FailText(cam.error().message);
    }
    Refresh();
  }));
}

void CallPresenter::ToggleSpeaker() {
  if (!CallAudioSession::SupportsSpeakerToggle()) {
    return;
  }
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available() || !backend->Media().IsActive()) {
    return;
  }
  const bool before = CallAudioSession::IsSpeakerphoneOn();
  CallAudioSession::SetSpeakerphoneOn(!before);
  // Speaker = route only (not mute). Android AudioRecord often goes silent until SDL reopen.
  const bool reopen = CallAudioSession::SpeakerToggleNeedsDeviceReopen();
  if (reopen) {
    backend->Media().RequestAudioDeviceReopen();
  }
  log().info << "ToggleSpeaker speaker_on=" << (!before ? 1 : 0) << " reopen=" << (reopen ? 1 : 0);
  Refresh();
}

} // namespace pbr
