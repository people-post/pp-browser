#include "feature/presenters/CallPresenter.h"

#include "feature/presenters/CallUiAttachGuard.h"
#include "feature/presenters/NewCallGuard.h"
#include "foundation/runtime/AppLifecycle.h"
#include "lib/calls/CallGuests.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallMediaErrorText.h"
#include "lib/calls/CallTypes.h"
#include "lib/media/AudioSession.h"
#include "lib/calls/CallMediaAdaptation.h"
#include "lib/media/IMediaView.h"
#include "feature/calls/CallUiBackend.h"
#include "common/Metrics.h"
#include "lib/pricing/PricingTypes.h"
#include "common/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** Remote frames this fresh mean the peer's camera is on, whatever its roster row says (B58). */
constexpr int64_t kPeerVideoLiveMs = 500;

int64_t SteadyMs(const PendingNotificationTap::Clock::time_point at) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

/**
 * Whether a tapped call alert can be for the call `call_id`: its hint names that call, or names none — as a ring
 * routes a tap with no hint (PendingNotificationTap::TakeCallRoute). A call id never fits a push hint today
 * (SanitizeWakeHint drops its ':'), so in practice every call tap has none.
 */
bool TapMayBeFor(const std::string& hint, const std::string& call_id) {
  return hint.empty() || hint == call_id;
}

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
    ForgetFailedPage("locked");
    return;
  }
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    bound_calls_ = nullptr;
    ForgetFailedPage("calls unavailable");
    return;
  }
  // CallSessionManager is recreated in BuildMessagingStack; rebind when the pointer changes.
  const void* identity = backend->SessionsIdentity();
  if (bound_calls_ == identity) {
    return;
  }
  if (bound_calls_ != nullptr) {
    ForgetFailedPage("call stack rebuilt"); // another account, or the stack built again
  }
  // Ingest may run on IO; the screen is decided on UI.
  backend->SetOnRingChanged([this]() { AppRuntime::PostUI([this]() { Refresh(); }); });
  backend->SetOnChromeRefresh([this]() { Refresh(); });
  backend->SetOnInviteBlocked([this](const BlockedCallInvite& blocked) { OnInviteBlocked(blocked); });
  bound_calls_ = identity;
  // Pick up post-restart abandon / pending ring after stack rebuild.
  Refresh();
}

void CallPresenter::ForgetFailedPage(const char* why) {
  redial_pending_since_ms_ = 0;
  if (failed_page_) {
    log().info << "kept failed page dropped (" << why << ") call_id=" << failed_page_->call_id;
    failed_page_.reset();
    Changed();
  }
}

void CallPresenter::NoteMessagingReady() {
  messaging_ready_ = true;
}

CallPresenter::WakeStep CallPresenter::StepWakePlaceholder(const bool phone_layout,
                                                           const PendingNotificationTap::Clock::time_point now) {
  WakeStep step;
  if (!phone_layout) {
    if (std::exchange(wake_ring_shown_, false)) {
      step.ring = WakeRing::Clear;
    }
    return step;
  }
  auto& tap = PendingNotificationTap::Instance();
  CallWakePlaceholder::Input in;
  in.tap_seq = tap.CallTapSeq();
  in.tap_fresh = tap.PeekCallRoute(now);
  if (const auto age = tap.CallTapNotificationAge()) {
    in.tap_notification_age_ms = age->count();
  }
  if (const auto elapsed = tap.CallTapElapsed(now)) {
    in.tap_elapsed_ms = elapsed->count();
  }
  in.app_visible = AppLifecycle::IsVisible();
  in.screen_kind = screen_.kind;
  in.messaging_ready = messaging_ready_;
  in.now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  PruneBlocked(in.now_ms);
  std::optional<BlockedCallInvite> blocked_tap;
  if (in.tap_fresh && !wake_blocks_.empty()) {
    const std::string hint = tap.FreshHint(now);
    for (const auto& [call_id, blocked] : wake_blocks_) {
      if (TapMayBeFor(hint, call_id)) {
        blocked_tap = blocked;
        break;
      }
    }
  }
  in.blocked = blocked_tap.has_value();
  const CallWakePlaceholder::View view = wake_placeholder_.Step(in);
  if (view.outcome == CallWakePlaceholder::Outcome::Blocked && blocked_tap) {
    // The page closes (or never shows) for a call that will not ring: it never goes without a word.
    wake_blocks_.erase(blocked_tap->call_id);
    blocked_pill_at_[blocked_tap->sender_identity] = in.now_ms;
    NotifyBlocked(*blocked_tap);
  }
  step.outcome = view.outcome;
  step.shown_ms = view.shown_ms;
  step.ready_ms = view.ready_ms;
  if (view.active) {
    wake_ring_shown_ = screen_.kind == CallScreen::Kind::None;
    step.ring = wake_ring_shown_ ? WakeRing::Show : WakeRing::Keep;
  } else if (view.outcome != CallWakePlaceholder::Outcome::Ring && std::exchange(wake_ring_shown_, false)) {
    step.ring = WakeRing::Clear; // over with no call: the page goes quietly (a real ring overwrote it in place: Keep)
  } else {
    wake_ring_shown_ = false;
  }
  return step;
}

void CallPresenter::PruneBlocked(const int64_t now_ms) {
  std::erase_if(blocked_calls_, [now_ms](const auto& seen) { return now_ms - seen.second > kDefaultCallInviteTtlMs; });
  std::erase_if(wake_blocks_, [this](const auto& wake) { return !blocked_calls_.contains(wake.first); });
  std::erase_if(blocked_pill_at_,
                [now_ms](const auto& pill) { return now_ms - pill.second >= kBlockedPillPerCallerMs; });
}

void CallPresenter::OnInviteBlocked(const BlockedCallInvite& blocked,
                                    const PendingNotificationTap::Clock::time_point now) {
  // Someone the user blocked (their texts are dropped unread too), or our own account: nothing at all.
  if (ports_.local_relay_identity && ports_.local_relay_identity() == blocked.sender_identity) {
    return;
  }
  if (ports_.find_contact_by_identity) {
    const auto contact = ports_.find_contact_by_identity(blocked.sender_identity, mesh::ContactIdKind::Account);
    if (contact && *contact &&
        ((*contact)->local.trust == TrustLevel::Blocked || (*contact)->trust == TrustLevel::Blocked)) {
      return;
    }
  }
  const int64_t now_ms = SteadyMs(now);
  PruneBlocked(now_ms);
  if (!blocked_calls_.emplace(blocked.call_id, now_ms).second) {
    return; // the same invite again (two paths, a redelivery)
  }
  if (blocked.from_group) {
    return; // a group's call: nothing shown, nothing recorded (as before)
  }
  log().info << "call blocked by who-can-call-me call_id=" << blocked.call_id << " expired=" << blocked.expired;
  if (ports_.record_blocked_call) {
    ports_.record_blocked_call(blocked);
  }
  if (blocked.expired) {
    return; // long over: the chat's line is all
  }
  wake_blocks_[blocked.call_id] = blocked;
  if (wake_ring_shown_ && TapMayBeFor(PendingNotificationTap::Instance().FreshHint(now), blocked.call_id)) {
    return; // the connecting page waiting for it closes on the next step, with the pill
  }
  if (!AppLifecycle::IsUserAttentive() || blocked_pill_at_.contains(blocked.sender_identity)) {
    return;
  }
  blocked_pill_at_[blocked.sender_identity] = now_ms;
  NotifyBlocked(blocked);
}

void CallPresenter::NotifyBlocked(const BlockedCallInvite& blocked) {
  CallNotice notice;
  notice.kind = CallNotice::Kind::CallBlocked;
  notice.identity = blocked.sender_identity;
  notice.key = blocked.audience == mesh::InboundAudience::Nobody ? "call.blocked.pill.nobody"
                                                                 : "call.blocked.pill.contacts_only";
  Notify(notice);
}

void CallPresenter::Tick() {
  BindToMessaging();
  if (RedialPending() && steady_ms_() - redial_pending_since_ms_ >= kRedialPendingMs) {
    log().warning << "call again neither showed its call nor failed in " << kRedialPendingMs / 1000 << " s";
    redial_pending_since_ms_ = 0;
  }
  auto* backend = Backend();
  if (!backend) {
    return;
  }
  backend->SweepExpiredInvites();
  const auto state = backend->State(); // one snapshot for this tick's decision
  ForgetLeavingOnceLetGo(*state);
  const EngineCall engine = EngineCallOf(*state);
  // Also while the engine has a call this screen lost — never the one the user just hung up: the refresh takes it back
  // (DEV/PLAN-call-ui-never-detached.md 3).
  if (state->available && (!active_call_id_.empty() || (engine.open && !Leaving(engine.call_id)))) {
    Refresh(); // Calling… / the timer / levels stay fresh, before media starts too
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
  if (auto media_err = backend.TakeLastMediaError(); media_err && !media_err->message.empty()) {
    // A dropped call or a connect timeout (its kind, K014) is told by the call screen's own failed page (#181:
    // one prompt, one place); the raw reason goes to the log. Other path failures are a plain toast, never a
    // banner that stays.
    if (CallMediaErrorShownOnCallScreen(*media_err)) {
      log().info << "call media error shown on the call screen only raw=" << media_err->message;
    } else {
      CallNotice notice;
      notice.kind = CallNotice::Kind::Error;
      notice.text = media_err->message;
      Notify(notice);
    }
  }
  // The peer ended a call we were in (also a failed one — failed is not closed): the panel is
  // about to go, so say why instead of letting the call silently vanish.
  if (auto ended = backend.TakeRemoteEnd(); ended) {
    if (KeepsFailedPage(screen_, ended->call_id, ended->declined)) {
      // A call that never connected, ended over there (their Try later, or the first step of their Call again): this
      // device's page stays as it was, with no call behind it, until the user taps (DEV/PLAN-call-failed-redial.md 6).
      failed_page_ = FailedPageOf(screen_);
      log().info << "the other side ended a call that never connected; its failed page stays call_id="
                 << ended->call_id;
      return true;
    }
    if (!ended->declined && ended->call_id == unconnected_call_id_) {
      // The same call, its page already gone here (Call again was tapped in that frame): nothing to tell either.
      log().info << "the other side ended a call that never connected; no pill call_id=" << ended->call_id;
      return true;
    }
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
  const auto state = backend->State();
  ForgetLeavingOnceLetGo(*state);
  CallUiBackend::Surface surface = backend->Settle();
  using SurfaceKind = CallUiBackend::Surface::Kind;
  if (surface.active && Leaving(surface.active->call_id)) {
    // Hung up here, its leave still on the owner's queue (the row still reads joined): it never comes back on screen.
    surface.active.reset();
    if (surface.kind == SurfaceKind::InCall) {
      surface.kind = SurfaceKind::None;
    }
  }
  if (surface.kind == SurfaceKind::Ringing && surface.active && surface.invite) {
    std::string failed_peer;
    if (auto peer = backend->PeerIdentityForCall(surface.active->call_id); peer && peer->has_value()) {
      failed_peer = **peer;
    }
    if (EndsFailedCallForRing(screen_, surface.active->call_id, *surface.invite, failed_peer, RedialPending())) {
      // Their Call again overtook the failed call's leave: the failed call ends here, the ring is a plain one.
      log().info << "the failed call's peer calls again: ending call_id=" << surface.active->call_id
                 << " for call_id=" << surface.invite->call_id;
      (void)Leave();
      Refresh(); // the failed call is being left now: the ring shows on its own
      return;
    }
  }
  const EngineCall engine = EngineCallOf(*state);
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
    next.ring_group = top.origin_group_id.has_value();
    next.thread_id = top.origin_thread_id.value_or(std::string{});
    // The Connecting bar has the speaker button too: the choice may be made before media opens.
    next.show_speaker = AudioSession::SupportsSpeakerToggle();
    next.speaker_on = SpeakerShownOn();
  } else if (surface.kind == SurfaceKind::Ringing) {
    const PendingCallInvite& top = *surface.invite;
    ringing_call_id_ = top.call_id;
    next.kind = CallScreen::Kind::Ringing;
    next.ring_call_id = top.call_id;
    next.caller_identity = top.inviter_identity;
    next.ring_video = top.video_allowed;
    next.ring_group = top.origin_group_id.has_value();
    next.thread_id = top.origin_thread_id.value_or(std::string{});
    if (surface.active) {
      // Ringing over the call we are in: answering ends that one. Keep it as the active call.
      active_call_id_ = surface.active->call_id;
      FillEndingCall(next, *backend, *surface.active);
    }
    // P001: waive / take-all when the inviter offered a positive initiation amount.
    next.offer_minor = backend->InitiationOfferMinorForPeer(top.inviter_identity);
    next.charge_enabled = next.offer_minor > 0 && PaymentRailsAvailable();
    FillGuestRing(next, *backend, top,
                  ports_.local_relay_identity ? ports_.local_relay_identity().value_or(std::string{}) : std::string{});
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
      notice.conflict = next.conflict; // V064: its alert says answering ends the call we are in
      notice.ending = next.ending;
      Notify(notice);
    }
  } else if (surface.kind == SurfaceKind::InCall) {
    active_call_id_ = surface.active->call_id;
    ringing_call_id_.clear();
    ring_shown_ = false;
    FillInCall(next, *backend, surface.active->call_id, surface.active->video_allowed,
               surface.active->origin_group_id.has_value());
    next.thread_id = surface.active->origin_thread_id.value_or(std::string{});
    const std::string local =
        ports_.local_relay_identity ? ports_.local_relay_identity().value_or(std::string{}) : std::string{};
    next.placed_here = !local.empty() && surface.active->initiator_identity == local;
    active_group_id_ = surface.active->origin_group_id.value_or(std::string{});
    FillGuests(next, *backend, active_group_id_, local);
    AnnounceGuests(next);
    FillGroupInvite(next, util::NowUnixMs());
    MaybeAutoCamera(*backend, next);
  } else if (engine.open && !Leaving(engine.call_id)) {
    // The store shows no call (a failed read) while the engine is still in one: the mic may be open, so its screen
    // stays — and with it the "in a call" notification and Hang up (DEV/PLAN-call-ui-never-detached.md 2).
    KeepEngineCall(next, *backend, engine);
  } else {
    active_call_id_.clear();
    active_group_id_.clear();
    ringing_call_id_.clear();
    ring_shown_ = false;
  }
  const int64_t now = util::NowUnixMs();
  // The screen's call went away by itself only when nothing replaced it and the stack is not still in it
  // (a ring over it, a declined ring, a merged call keep or replace it).
  const bool went_away =
      next.kind == CallScreen::Kind::None && (!surface.active || surface.active->call_id != screen_.call_id);
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
  if (failed_page_ && next.kind != CallScreen::Kind::None &&
      !(next.kind == CallScreen::Kind::InCall && next.call_id == failed_page_->call_id)) {
    log().info << "kept failed page replaced by " << CallScreenKindName(next.kind)
               << " call_id=" << failed_page_->call_id;
    failed_page_.reset(); // another call, or a ring, takes the screen
  }
  // The 1:1 call this device is in that has not connected (KeepsFailedPage's), remembered past its page.
  if (const std::string& shown = next.kind == CallScreen::Kind::Answering ? next.ring_call_id : next.call_id;
      KeepsFailedPage(next, shown, false)) {
    unconnected_call_id_ = shown;
  } else if (next.kind == CallScreen::Kind::InCall && next.call_id == unconnected_call_id_) {
    unconnected_call_id_.clear(); // it connected (or became a group's)
  }
  if (RedialPending() && next.kind != CallScreen::Kind::None &&
      (next.kind != CallScreen::Kind::InCall || next.call_id != redial_from_call_id_)) {
    redial_pending_since_ms_ = 0; // the new call's page (or whatever took the screen) is up
  }
  WatchForNoMedia(*backend, next, now);
  OfferCallBack(next, surface);
  StepMedia(*backend, next, now);
  if (next.kind == CallScreen::Kind::None && !surface.active) {
    speaker_tap_.reset(); // the engine dropped a route change not yet run with the call's media
  }
  if (next.kind == CallScreen::Kind::None && !surface.active && AudioSession::IsSpeakerphoneOn()) {
    // No call left: the speaker choice was that call's (AudioSession keeps it across a media rebuild). Forgotten, not
    // routed from here: on Android the call's mic may still be open (its engine closes it on its own thread), and a
    // route change under an open recording stream loses the mic for the process; its Deactivate routes back after.
    AudioSession::ForgetSpeakerChoice();
  }
  screen_ = std::move(next);
  if (dev_detach_ui_ && !active_call_id_.empty() && dev_detach_ui_()) {
    if (std::exchange(dev_detached_call_id_, active_call_id_) != active_call_id_) {
      log().warning << "call screen let go of its call on purpose (debug.pp.detach_call_ui) call_id="
                    << active_call_id_;
    }
    active_call_id_.clear(); // the screen stays; only the id goes, as if something else had dropped it
  }
  Changed();
}

void CallPresenter::KeepEngineCall(CallScreen& next, CallUiBackend& backend, const EngineCall& engine) {
  const std::string& call_id = engine.call_id;
  if (kept_call_logged_ != call_id) {
    kept_call_logged_ = call_id;
    // Read once more for the log only (once per call): was it a failed read, or no row?
    auto read = backend.ActiveLocalCall();
    log().warning << "call kept on screen: the store shows none while the engine is in it phase="
                  << CallPhaseName(engine.phase) << " read="
                  << (!read               ? "error"
                      : read->has_value() ? "found"
                                          : "none")
                  << (read ? std::string() : " err=" + read.error().message)
                  << " had_call=" << (active_call_id_ == call_id ? 1 : 0) << " call_id=" << call_id;
  }
  if (active_call_id_ == call_id && screen_.kind == CallScreen::Kind::InCall && screen_.call_id == call_id) {
    next = screen_; // the screen as it was
  } else {
    // What the engine alone can say (the store's names and roster may be unreadable): a plain in-call screen with End.
    FillInCall(next, backend, call_id, /*video=*/false, /*group_call=*/false);
  }
  active_call_id_ = call_id;
  ringing_call_id_.clear();
  ring_shown_ = false;
}

bool CallPresenter::Leaving(const std::string& call_id) const {
  return !call_id.empty() && call_id == leaving_call_id_;
}

void CallPresenter::ForgetLeavingOnceLetGo(const CallUiState& state) {
  if (leaving_call_id_.empty()) {
    return;
  }
  // Let go: its leave ran (Idle), or the engine shows another call, or the calls are gone.
  if (!state.available || state.phase == CallPhase::Idle || state.active_call_id != leaving_call_id_) {
    log().info << "hung-up call let go by the engine call_id=" << leaving_call_id_;
    leaving_call_id_.clear();
    return;
  }
  if (steady_ms_() - leaving_since_ms_ >= kLeavingLatchMs) {
    // Never forever: the guard takes a call still open after this as lost again.
    log().warning << "hung-up call still open in the engine after " << kLeavingLatchMs / 1000
                  << " s; no longer kept off screen call_id=" << leaving_call_id_;
    leaving_call_id_.clear();
  }
}

void CallPresenter::StartLeaving(const std::string& call_id) {
  leaving_call_id_ = call_id;
  leaving_since_ms_ = steady_ms_();
}

void CallPresenter::StopLeaving(const char* why) {
  if (!leaving_call_id_.empty()) {
    log().info << "hung-up call no longer kept off screen (" << why << ") call_id=" << leaving_call_id_;
    leaving_call_id_.clear();
  }
}

void CallPresenter::SetSteadyClockForTest(std::function<int64_t()> now_ms) {
  steady_ms_ = std::move(now_ms);
}

void CallPresenter::SetDevDetachUi(std::function<bool()> detach) {
  dev_detach_ui_ = std::move(detach);
}

void CallPresenter::FillEndingCall(CallScreen& s, CallUiBackend& backend, const CallSession& active) {
  s.conflict = true;
  CallScreen::EndingCall& ending = s.ending;
  ending.call_id = active.call_id;
  ending.group = active.origin_group_id.has_value();
  ending.thread_id = active.origin_thread_id.value_or(std::string{});
  ending.video = active.video_allowed;
  if (auto peer = backend.PeerIdentityForCall(active.call_id); peer && peer->has_value()) {
    ending.peer_identity = **peer;
    ending.same_peer = **peer == s.caller_identity;
  }
}

void CallPresenter::OfferCallBack(const CallScreen& next, const CallUiBackend::Surface& surface) {
  const std::string live = surface.active ? surface.active->call_id : std::string();
  if (auto offer = call_back_.OnFrame(next, live)) {
    CallNotice notice;
    notice.kind = CallNotice::Kind::CallBack;
    notice.identity = offer->peer_identity;
    notice.ending = std::move(*offer);
    Notify(notice);
  }
}

void CallPresenter::FillInCall(CallScreen& s, CallUiBackend& backend, const std::string& call_id, const bool video,
                               const bool group_call) {
  s = {};
  s.kind = CallScreen::Kind::InCall;
  s.call_id = call_id;
  s.video = video;
  s.muted = backend.Media().IsMuted();
  s.show_speaker = AudioSession::SupportsSpeakerToggle();
  s.speaker_on = SpeakerShownOn();
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
      member.on_hold = row.media.on_hold;
      member.video_paused = row.media.video_paused;
      s.members.push_back(std::move(member));
    }
  }
  if (auto peer = backend.PeerIdentityForCall(call_id); peer && peer->has_value()) {
    s.peer_identity = **peer;
  }

  InCallSignals signals;
  signals.direct_connect_failed = backend.IsDirectConnectFailed();
  signals.direct_missing_mic = backend.DirectConnectMissingMic();
  signals.direct_seed_unreachable = backend.DirectConnectSeedUnreachable();
  // The hop path being recovered, also a guest's lost transport between reattach attempts (the
  // Reconnecting line stays up; the seat stays Live by design).
  signals.recovery = backend.Recovery();
  signals.on_hop = backend.MediaPath() == CallMediaPath::Hop;
  signals.group_context = group_call || joined > 2 || signals.recovery != CallRecovery::None || signals.on_hop;
  signals.media_chrome_live = backend.MediaChromeLive();
  signals.media_status = backend.MediaStatus();
  signals.media_active = backend.Media().IsActive();
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
  s.show_make_group = OffersMakeGroup(group_call, static_cast<size_t>(joined));
  s.hint = decided.hint;
  s.hint_missing_mic = signals.direct_missing_mic;
  s.hint_seed_unreachable = signals.direct_seed_unreachable;
  if (decided.status == CallScreen::Status::Activity) {
    s.activity = signals.activity;
  }
  if (const int64_t alone_until = backend.AloneUntilMs(); s.group && alone_until > 0) {
    // V053: everyone else left; the line counts down to this device leaving by itself.
    s.status = CallScreen::Status::WaitingForOthers;
    s.alone_until_ms = alone_until;
  }
  if (decided.show_elapsed || ShowsParticipantCount(s)) {
    s.connected_at_ms = live_since_ms; // the call's, not the current path's
  }
}

void CallPresenter::FillGroupInvite(CallScreen& s, const int64_t now) {
  if (group_invites_call_id_ != s.call_id) {
    // Another call: what was read and put off belonged to the last one.
    group_invites_call_id_ = s.call_id;
    group_invites_.clear();
    group_invites_put_off_.clear();
    group_invite_joining_.clear();
    group_invites_read_ms_ = 0;
  }
  // Refresh runs every frame in a call; the invites change only when one arrives or is answered.
  if (ports_.group_invites_in_call && (group_invites_read_ms_ == 0 || now - group_invites_read_ms_ >= 1'000)) {
    group_invites_read_ms_ = now;
    group_invites_ = ports_.group_invites_in_call(s.call_id);
  }
  std::set<std::string> hidden = group_invites_put_off_;
  if (!group_invite_joining_.empty()) {
    hidden.insert(group_invite_joining_);
  }
  s.group_invite = PickGroupInviteBanner(group_invites_, hidden);
}

void CallPresenter::WatchForNoMedia(CallUiBackend& backend, const CallScreen& s, const int64_t now) {
  // Once a second is plenty (the health snapshot takes the engine's lock), and only a group call counts.
  if (now - no_media_checked_ms_ < 1'000) {
    return;
  }
  no_media_checked_ms_ = now;
  NoMediaSample sample;
  if (s.kind == CallScreen::Kind::InCall && s.group) {
    const MediaEngineHealth health = backend.Media().HealthSnapshot();
    sample.call_id = s.call_id;
    sample.group = s.group;
    // A hop reattach keeps the seat "live" while nothing flows: recovering pauses the count too.
    sample.media_live =
        s.media_connected && backend.Recovery() == CallRecovery::None && s.status != CallScreen::Status::Reconnecting;
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
  // Deliberately the path rule (not ShowsCameraButton, which keeps the button for a camera that is on):
  // never auto-turn the camera on under path pressure.
  const bool camera_offerable =
      backend.Media().VideoEncoderAvailable() &&
      CallMediaAdaptation::CameraPathAllowsVideo(backend.Media().PathPressure(), backend.Media().UplinkBudget());
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
                                       logging::getLogger("CallPresenter").warning
                                           << "auto camera on failed call_id=" << call_id << ": "
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

void CallPresenter::StepMedia(CallUiBackend& backend, const CallScreen& next, const int64_t now) {
  switch (next.kind) {
    case CallScreen::Kind::InCall: {
      if (auto failure = backend.Media().TakeCameraFailure()) {
        // Camera opens asynchronously (media device thread): report the miss. A camera that only waits for the device
        // (busy, or not responding yet) stays on and comes on by itself (L016); any other miss withdraws the video flag
        // the roster already advertised.
        ReportCameraFailure(*failure);
        if (!backend.Media().CameraWaitingForDevice()) {
          backend.SetLocalVideoEnabled(false);
        }
      }
      CallMediaFacts facts = ReadMediaFacts(backend, next, now);
      TrackPlayoutStuck(facts, next);
      TrackHoldStuck(facts, next);
      const CallMediaScreen& frame = media_frame_.Step(next, facts);
      if (!frame.pill_key.empty()) {
        CallNotice notice;
        notice.kind = CallNotice::Kind::Hint;
        notice.key = frame.pill_key;
        notice.alarm = frame.pill_alarm;
        Notify(notice);
      }
      ActOnMedia(backend, frame);
      LogMedia(facts, frame);
      return;
    }
    case CallScreen::Kind::Answering:
      media_frame_.StepAnswering(next, now);
      return;
    case CallScreen::Kind::Ringing:
      return; // a ring over the call: its media frame waits (the in-call chrome is hidden meanwhile)
    case CallScreen::Kind::None:
      if (active_call_id_.empty()) {
        media_frame_.Reset();
        playout_stuck_.Reset();
        playout_call_id_.clear();
        EmitHoldStuckEnd(hold_stuck_.Finish(), hold_group_);
        hold_call_id_.clear();
      }
      return;
  }
}

CallMediaFacts CallPresenter::ReadMediaFacts(CallUiBackend& backend, const CallScreen& s, const int64_t now) {
  IMediaView& media = backend.Media();
  media.RefreshRemoteVideoHealth(); // the engine's video upkeep, before the reads below
  CallMediaFacts f;
  f.call_id = s.call_id;
  f.now_ms = now;
  if (auto allowed = backend.VideoAllowedForCall(s.call_id); allowed && allowed->has_value()) {
    f.video_allowed = **allowed;
  }
  f.video_intent = !auto_camera_call_id_.empty() && auto_camera_call_id_ == s.call_id;
  f.ringing_out = backend.Phase() == CallPhase::OutboundCalling;
  f.media_chrome_live = backend.MediaChromeLive();
  f.media_status = backend.MediaStatus();
  f.direct_connect_failed = backend.IsDirectConnectFailed();
  f.awaiting_sfu_recovery = backend.IsAwaitingSfuRecovery();
  f.on_hop = backend.MediaPath() == CallMediaPath::Hop;
  f.live_since_ms = backend.LiveSinceMs(s.call_id);
  if (auto peer = backend.PeerMediaForCall(s.call_id); peer && peer->has_value()) {
    f.have_peer_row = true;
    f.peer_camera_on = (*peer)->video_enabled;
    f.peer_on_hold = (*peer)->on_hold;
    f.peer_video_paused = (*peer)->video_paused;
  }
  f.media_active = media.IsActive();
  f.camera_on = media.IsCameraEnabled();
  f.encoder_available = media.VideoEncoderAvailable();
  f.path_allows_video = CallMediaAdaptation::CameraPathAllowsVideo(media.PathPressure(), media.UplinkBudget());
  f.has_remote_video = media.HasRemoteVideo();
  f.remote_video_stalling = media.IsRemoteVideoStalling();
  f.peer_frames_live = media.IsRemoteVideoLive(kPeerVideoLiveMs);
  f.ever_had_remote_video = media.EverHadRemoteVideo();
  f.local_on_hold = callbacks_.on_hold_here && callbacks_.on_hold_here();
  f.mic_blocked = backend.DirectConnectMissingMic();
  f.engine = media.HealthSnapshot();
  f.hop = backend.HopHealth();
  f.reach_path_kind = backend.MediaPathKind();
  f.link_counters = backend.MediaLinkCounters();
  return f;
}

void CallPresenter::TrackPlayoutStuck(CallMediaFacts& facts, const CallScreen& s) {
  if (facts.call_id != playout_call_id_) {
    playout_call_id_ = facts.call_id;
    playout_stuck_.Reset();
  }
  using Status = CallScreen::Status;
  CallPlayoutStuck::Sample sample;
  sample.counting = facts.media_chrome_live && facts.media_active && !facts.local_on_hold &&
                    !(facts.have_peer_row && facts.peer_on_hold) &&
                    facts.media_status != CallMediaStatus::Reconnecting && !facts.direct_connect_failed &&
                    !facts.awaiting_sfu_recovery && s.status != Status::Reconnecting &&
                    s.status != Status::Disconnected && s.status != Status::CouldntConnect;
  sample.rx_frames = facts.engine.rx_audio_frames;
  sample.underruns = facts.engine.playout_underruns;
  sample.jitter_depth_ms = facts.engine.jitter_depth_ms;
  sample.now_ms = facts.now_ms;
  const CallPlayoutStuck::Result result = playout_stuck_.Update(sample);
  facts.playout_stuck = result.stuck;
  if (result.became_stuck) {
    static constexpr ShareWord kKinds[] = {"one_to_one", "group"};
    const int64_t after_s =
        facts.live_since_ms > 0 ? std::max<int64_t>(0, facts.now_ms - facts.live_since_ms) / 1000 : 0;
    log().warning << "playout stuck call_id=" << facts.call_id << " group=" << s.group << " after_s=" << after_s;
    MetricsLine("call.playout_stuck")
        .Pick("kind", s.group ? "group" : "one_to_one", kKinds)
        .Add("after_s", after_s)
        .Emit();
  }
}

void CallPresenter::TrackHoldStuck(CallMediaFacts& facts, const CallScreen& s) {
  if (facts.call_id != hold_call_id_) {
    EmitHoldStuckEnd(hold_stuck_.Finish(), hold_group_);
    hold_call_id_ = facts.call_id;
  }
  hold_group_ = s.group;
  using Status = CallScreen::Status;
  CallHoldStuck::Sample sample;
  sample.held = facts.local_on_hold;
  sample.foreground = !callbacks_.app_foreground || callbacks_.app_foreground();
  sample.reconnecting = facts.media_status == CallMediaStatus::Reconnecting || facts.direct_connect_failed ||
                        facts.awaiting_sfu_recovery || s.status == Status::Reconnecting ||
                        s.status == Status::Disconnected || s.status == Status::CouldntConnect;
  sample.now_ms = facts.now_ms;
  const CallHoldStuck::Result result = hold_stuck_.Update(sample);
  facts.hold_stuck = result.stuck;
  if (result.became_stuck) {
    log().warning << "hold stuck call_id=" << facts.call_id << " group=" << s.group;
  }
  EmitHoldStuckEnd(result, s.group);
}

void CallPresenter::EmitHoldStuckEnd(const CallHoldStuck::Result& result, const bool group) {
  if (!result.ended) {
    return;
  }
  static constexpr ShareWord kKinds[] = {"one_to_one", "group"};
  static constexpr ShareWord kEnds[] = {"recovered", "call_ended"};
  log().info << "hold stuck over group=" << group << " held_s=" << result.counted_ms / 1000
             << " end=" << (result.end == CallHoldStuck::End::Recovered ? "recovered" : "call_ended");
  MetricsLine("call.hold_stuck")
      .Pick("kind", group ? "group" : "one_to_one", kKinds)
      .Add("held_s", result.counted_ms / 1000)
      .Pick("end", result.end == CallHoldStuck::End::Recovered ? "recovered" : "call_ended", kEnds)
      .Emit();
}

void CallPresenter::ActOnMedia(CallUiBackend& backend, const CallMediaScreen& frame) {
  if (frame.media.clear_remote_video) {
    backend.Media().ClearRemoteVideo();
  }
  if (frame.media.narrow_camera) {
    // V035: narrowed to voice with the camera on — off through the normal path, once.
    log().info << "camera off (video_allowed=false) call_id=" << frame.call_id;
    backend.SetLocalVideoEnabled(false, WhileAlive([this](Roe<void> cam) {
                                   if (!cam) {
                                     FailText(cam.error().message);
                                   }
                                 }));
  }
}

void CallPresenter::LogMedia(const CallMediaFacts& facts, const CallMediaScreen& frame) {
  const char* roster = !facts.have_peer_row ? "none" : facts.peer_camera_on ? "on" : "off";
  const char* frames = facts.peer_frames_live ? "live" : "idle";
  if (std::string key = std::string(roster) + "/" + frames; key != peer_video_log_key_) {
    log().info << "peer video call_id=" << facts.call_id << " roster=" << roster << " frames=" << frames;
    peer_video_log_key_ = std::move(key);
  }
  if (frame.call_id != story_logged_call_) {
    story_logged_call_ = frame.call_id;
    story_events_logged_ = 0;
  }
  // The story's moments with the technical words the screen never shows (TODO #76: the detail goes to the log).
  const std::vector<CallStoryEvent>& events = frame.story.events;
  for (size_t i = story_events_logged_; i < events.size(); ++i) {
    log().info << "call story call_id=" << frame.call_id << " event=" << static_cast<int>(events[i].kind)
               << " at_ms=" << events[i].at_ms << " path=" << frame.health.path_kind;
  }
  story_events_logged_ = events.size();
}

// --- buttons ------------------------------------------------------------------------------------

bool CallPresenter::GuardNewCall(const std::string& thread_id, const std::function<void()>& place,
                                 const StartDone& done) {
  const NewCallVerdict verdict = DecideNewCall(screen_, thread_id);
  if (verdict == NewCallVerdict::Place) {
    if (CallScreenIsFailedPage(screen_)) {
      (void)Leave(); // clear the failed page (shown or minimized) and its dead call before the new one
    }
    return false;
  }
  const auto finish = [done](const CallStartResult::Kind kind) {
    if (done) {
      CallStartResult result;
      result.kind = kind;
      done(result);
    }
  };
  NewCallSteps steps;
  if (callbacks_.confirm_end_current_call) {
    steps.ask = [this](std::function<void(bool)> answer) { callbacks_.confirm_end_current_call(std::move(answer)); };
  }
  steps.show_current = [this, finish]() {
    if (callbacks_.show_current_call) {
      callbacks_.show_current_call();
    }
    finish(CallStartResult::Kind::Cancelled);
  };
  steps.leave_current = [this]() { (void)Leave(); };
  steps.place = place;
  steps.cancelled = [finish]() { finish(CallStartResult::Kind::Cancelled); };
  RunNewCall(verdict, steps);
  return true;
}

bool ChatCallAsksVideo(const bool call_joinable, const bool want_video, const bool video_available) {
  return !call_joinable && want_video && video_available;
}

bool CallPresenter::CallFromChat(const std::string& thread_id, const bool want_video, StartDone done) {
  if (thread_id.empty()) {
    return false;
  }
  const bool video = ChatCallAsksVideo(CallJoinableInThread(thread_id), want_video, want_video && VideoCallAvailable());
  return StartCall(thread_id, video, std::move(done));
}

bool CallPresenter::StartCall(const std::string& thread_id, const bool video, StartDone done) {
  if (GuardNewCall(thread_id, [this, thread_id, video, done]() { (void)StartCall(thread_id, video, done); }, done)) {
    return true;
  }
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
      RequestStart(thread_id, false, {}, std::move(done)); // V053: the group's call goes on — join it
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
  if (GuardNewCall(
          thread_id,
          [this, thread_id, video, invitees, done]() { (void)StartCallWithInvitees(thread_id, video, invitees, done); },
          done)) {
    return true;
  }
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
  StopLeaving("a call started or joined"); // joining the group's call just left gets it back on screen
  // No invitees: joining the group's call that goes on (not an outbound call).
  const bool joining = invitees.empty();
  // The calls owner starts it and reports back on UI.
  Backend()->StartCall(thread_id, video, invitees,
                       WhileAlive([this, video, joining, done = std::move(done)](Roe<CallSession> started) {
                         CallStartResult result;
                         if (!started) {
                           if (!redial_peer_.empty() && RingingFrom(redial_peer_)) {
                             // Call again lost to their own call ringing here: that ring is what the user wanted.
                             log().info << "call again refused while their call rings: " << started.error().message;
                           } else {
                             FailText(started.error().message);
                           }
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

CallJoinBar MakeCallJoinBar(const std::optional<CallUiBackend::JoinableCallInfo>& call) {
  CallJoinBar bar;
  if (!call) {
    return bar;
  }
  bar.shown = true;
  bar.video = call->media_mode == CallMediaMode::Video;
  bar.joined = call->joined_count && *call->joined_count > 0 ? *call->joined_count : 0;
  bar.text_key = bar.video ? "chat.call_bar.video" : "chat.call_bar.voice";
  return bar;
}

CallJoinBar CallPresenter::JoinBarInThread(const std::string& thread_id) const {
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return {};
  }
  return MakeCallJoinBar(backend->JoinableCallInfoInThread(thread_id));
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

void CallPresenter::OpenMakeGroup() {
  if (screen_.kind != CallScreen::Kind::InCall || !screen_.show_make_group || screen_.call_id.empty()) {
    return; // a stale tap: the call ended, or is not one a group may be made from
  }
  if (callbacks_.open_make_group) {
    callbacks_.open_make_group(screen_.call_id);
  }
}

void CallPresenter::JoinGroupInvite(std::function<void(Roe<void>)> done) {
  if (!screen_.group_invite || !ports_.join_group_invite) {
    if (done) {
      done(Error("No group invite in this call"));
    }
    return;
  }
  const std::string invite_id = screen_.group_invite->invite_id;
  group_invite_joining_ = invite_id;
  Refresh();
  ports_.join_group_invite(invite_id, WhileAlive([this, invite_id, done](Roe<void> joined) {
                             if (group_invite_joining_ == invite_id) {
                               group_invite_joining_.clear();
                             }
                             if (!joined) {
                               FailText(joined.error().message);
                             }
                             group_invites_read_ms_ =
                                 0; // read again: an accepted invite is gone, a failed one back on the banner
                             Refresh();
                             if (done) {
                               done(joined);
                             }
                           }));
}

void CallPresenter::PutOffGroupInvite() {
  if (!screen_.group_invite) {
    return;
  }
  group_invites_put_off_.insert(screen_.group_invite->invite_id);
  Refresh();
}

void CallPresenter::FillGuests(CallScreen& s, CallUiBackend& backend, const std::string& group_id,
                               const std::string& local) {
  if (group_id.empty() || !backend.ChecksGuests()) {
    return; // V062 6: a call with no group has no guests
  }
  const std::optional<CallGroupStanding> self = backend.GroupStanding(group_id, local);
  // V062 2: a guest's device (no roster) adds nobody; members add people (guests only if the preset lets them).
  s.show_invite = s.show_invite && self && self->member;
  std::map<std::string, std::string> brought_by_of;
  for (const CallParticipant& row : backend.ListParticipants(s.call_id).valueOr(std::vector<CallParticipant>{})) {
    brought_by_of[row.identity] = row.brought_by;
  }
  for (CallScreen::Member& member : s.members) {
    const std::string& brought_by = brought_by_of[member.identity];
    const std::optional<CallGroupStanding> standing = backend.GroupStanding(group_id, member.identity);
    member.guest = ShowsAsGuest(true, brought_by, standing);
    if (!member.guest) {
      continue;
    }
    member.brought_by = brought_by;
    member.can_remove = !member.is_local && MayRemoveFromCall(true, self, standing);
    member.can_add_to_group = !member.is_local && OffersGroupInvite(true, self, standing);
  }
}

void CallPresenter::FillGuestRing(CallScreen& s, CallUiBackend& backend, const PendingCallInvite& invite,
                                  const std::string& local) {
  if (!invite.origin_group_id || local.empty()) {
    return;
  }
  auto rows = backend.ListParticipants(invite.call_id);
  if (!rows) {
    return;
  }
  for (const CallParticipant& row : *rows) {
    if (row.identity == local) {
      s.ring_guest = !row.brought_by.empty();
    } else if (row.state == CallParticipantState::Joined) {
      ++s.ring_people;
    }
  }
  if (!s.ring_guest) {
    s.ring_people = 0;
  }
}

void CallPresenter::AnnounceGuests(const CallScreen& s) {
  if (announced_call_id_ != s.call_id) {
    announced_call_id_ = s.call_id;
    announced_guests_.clear();
  }
  for (const CallScreen::Member& member : s.members) {
    if (!member.guest || member.is_local || !announced_guests_.insert(member.identity).second) {
      continue;
    }
    CallNotice notice;
    notice.kind = CallNotice::Kind::GuestJoined;
    notice.identity = member.identity;
    notice.brought_by = member.brought_by;
    Notify(notice);
  }
}

bool CallPresenter::MayAddToActiveCall(const std::string& identity) const {
  auto* backend = Backend();
  if (!backend || !backend->Available() || active_group_id_.empty() || !backend->ChecksGuests()) {
    return true;
  }
  const std::string local =
      ports_.local_relay_identity ? ports_.local_relay_identity().value_or(std::string{}) : std::string{};
  return DecideCallAdd(true, backend->GroupStanding(active_group_id_, local),
                       backend->GroupStanding(active_group_id_, identity)) != CallAddVerdict::Refused;
}

void CallPresenter::RemoveFromCall(const std::string& identity, std::function<void(Roe<void>)> done) {
  auto* backend = Backend();
  if (!backend || !backend->Available() || active_call_id_.empty() || identity.empty()) {
    Fail("call.error.no_active");
    if (done) {
      done(Error("no call in progress"));
    }
    return;
  }
  backend->RemoveFromCall(active_call_id_, identity, WhileAlive([this, done](Roe<void> removed) {
                            if (!removed) {
                              FailText(removed.error().message);
                            }
                            Refresh();
                            if (done) {
                              done(removed);
                            }
                          }));
}

void CallPresenter::AddGuestToGroup(const std::string& identity, std::function<void(Roe<void>)> done) {
  if (active_call_id_.empty() || active_group_id_.empty() || identity.empty() || !ports_.invite_guest_to_group) {
    Fail("call.error.no_active");
    if (done) {
      done(Error("no group call in progress"));
    }
    return;
  }
  ports_.invite_guest_to_group(active_group_id_, identity, active_call_id_, WhileAlive([this, done](Roe<void> invited) {
                                 if (!invited) {
                                   FailText(invited.error().message);
                                 }
                                 if (done) {
                                   done(invited);
                                 }
                               }));
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
  if (video_available_for_test_) {
    return video_available_for_test_();
  }
  BindToMessaging();
  auto* backend = Backend();
  return backend && backend->Available() && backend->Media().VideoEncoderAvailable();
}

namespace {

/**
 * The click frame of Accept: `call_id` answering, with what the ring showed of it (the next Refresh fills in the rest).
 * Not an empty screen: the phone's call screen reads that as the ring gone and would end the call just answered there.
 */
CallScreen AnsweringFrom(const CallScreen& ring, const std::string& call_id) {
  CallScreen next;
  next.kind = CallScreen::Kind::Answering;
  next.ring_call_id = call_id;
  if (ring.ring_call_id == call_id) {
    next.caller_identity = ring.caller_identity;
    next.ring_video = ring.ring_video;
    next.ring_group = ring.ring_group;
    next.thread_id = ring.thread_id;
  }
  next.show_speaker = AudioSession::SupportsSpeakerToggle();
  next.speaker_on = AudioSession::IsSpeakerphoneOn(); // answering: media is not up, no route change can be waiting
  return next;
}

} // namespace

std::string CallPresenter::RingCallId(CallUiBackend& backend) const {
  if (!ringing_call_id_.empty()) {
    return ringing_call_id_;
  }
  if (!screen_.ring_call_id.empty()) {
    return screen_.ring_call_id;
  }
  return backend.LastRingCallId();
}

std::string CallPresenter::Accept(const bool voice_only, const std::string& answered) {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return {};
  }
  const std::string call_id = answered.empty() ? RingCallId(*backend) : answered;
  if (call_id.empty()) {
    log().warning << "Accept ignored (no call_id)";
    return {};
  }
  StopLeaving("a call answered");
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
    auto_camera_pending_ = false; // answered voice-only: never the camera for this call
  }
  // The ring goes on the click frame (CALLS.md Accept → Accepting): leaving it up until
  // AcceptInvite finished made Accept look hung.
  ringing_call_id_.clear();
  ring_shown_ = false;
  answered_call_id_ = call_id;
  call_back_.OnAccept(screen_, call_id); // End & accept over a 1:1 call: offer it again afterwards (V064)
  screen_ = AnsweringFrom(screen_, call_id);
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
  StopLeaving("a call answered");
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
  call_back_.OnAccept(screen_, call_id); // End & accept over a 1:1 call: offer it again afterwards (V064)
  screen_ = AnsweringFrom(screen_, call_id);
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
    // Until the owner ran the leave, the engine (and the row) still read open: never taken back meanwhile.
    StartLeaving(call_id);
  }
  if (RedialPending() && screen_.kind == CallScreen::Kind::None) {
    return {}; // a stale Try later after Call again: the failed call is being left and the new call is on its way
  }
  // No call: a stale End button after Idle — the screen still clears, so nothing looks hung. The kept failed page's "Try
  // later" is this: the page goes, nothing is sent.
  failed_page_.reset();
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

void CallPresenter::HangUpEngineCall(const std::string& call_id) {
  BindToMessaging();
  auto* backend = Backend();
  if (call_id.empty() || !backend || !backend->Available()) {
    return;
  }
  backend->StopCallMedia(call_id); // the engine's call by its id, whatever this screen holds
  StartLeaving(call_id);
  if (active_call_id_.empty() || active_call_id_ == call_id) {
    active_call_id_.clear();
    ringing_call_id_.clear();
    ring_shown_ = false;
    screen_ = {};
    Changed();
  }
  backend->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  if (!active_call_id_.empty()) {
    Refresh(); // this screen held another call: what it shows is decided again
  }
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
  Refresh(); // a refusal reads as the stack's last error, which Refresh reports
}

bool OffersRedial(const CallScreen& screen) {
  return screen.kind == CallScreen::Kind::InCall && screen.status == CallScreen::Status::CouldntConnect &&
         !screen.group;
}

bool KeepsFailedPage(const CallScreen& shown, const std::string& ended_call_id, const bool declined) {
  if (declined || ended_call_id.empty() || shown.thread_id.empty()) {
    return false;
  }
  if (shown.kind == CallScreen::Kind::Answering) {
    // Answered here, the steps still going (not in the call yet).
    return shown.ring_call_id == ended_call_id && !shown.ring_group && !shown.caller_identity.empty();
  }
  using Status = CallScreen::Status;
  const bool ringing_them = shown.placed_here && shown.status == Status::Calling; // they never answered
  return shown.kind == CallScreen::Kind::InCall && shown.call_id == ended_call_id && !shown.group &&
         !shown.was_connected && shown.status != Status::Disconnected && !ringing_them && !shown.peer_identity.empty();
}

CallFailedPage FailedPageOf(const CallScreen& shown) {
  CallFailedPage page;
  page.thread_id = shown.thread_id;
  if (shown.kind == CallScreen::Kind::Answering) {
    page.call_id = shown.ring_call_id;
    page.peer_identity = shown.caller_identity;
    page.video = shown.answered_video; // as answered: a video call answered by voice calls back by voice
    return page;
  }
  page.call_id = shown.call_id;
  page.peer_identity = shown.peer_identity;
  page.video = shown.video;
  page.hint_missing_mic = shown.hint_missing_mic;
  page.hint_seed_unreachable = shown.hint_seed_unreachable;
  return page;
}

bool EndsFailedCallForRing(const CallScreen& shown, const std::string& active_call_id, const PendingCallInvite& invite,
                           const std::string& failed_peer, const bool redial_pending) {
  if (redial_pending || !OffersRedial(shown) || shown.call_id != active_call_id || failed_peer.empty()) {
    return false;
  }
  const bool same_chat = invite.origin_thread_id.value_or(std::string{}) == shown.thread_id;
  return !invite.origin_group_id.has_value() && invite.call_id != active_call_id &&
         invite.inviter_identity == failed_peer && same_chat;
}

void CallPresenter::Redial() {
  if (RedialPending()) {
    return; // its new call is on its way
  }
  CallFailedPage page;
  const bool live_page = OffersRedial(screen_);
  if (live_page) {
    page.call_id = screen_.call_id;
    page.thread_id = screen_.thread_id;
    page.peer_identity = screen_.peer_identity;
    page.video = screen_.video;
  } else if (failed_page_ && screen_.kind == CallScreen::Kind::None) {
    page = *failed_page_;
  } else {
    return; // a stale tap: the page is gone
  }
  if (page.thread_id.empty()) {
    log().warning << "call again: the failed call has no chat call_id=" << page.call_id;
    return;
  }
  // The page's own kind: CallFromChat's "a call goes on in this chat" rule is not asked (the failed call is still on
  // record until its leave runs). Video this device cannot send goes voice, as from the chat.
  const bool video = ChatCallAsksVideo(/*call_joinable=*/false, page.video, page.video && VideoCallAvailable());
  redial_pending_since_ms_ = std::max<int64_t>(1, steady_ms_());
  redial_from_call_id_ = page.call_id;
  redial_peer_ = page.peer_identity;
  if (std::exchange(failed_page_, std::nullopt)) {
    Changed(); // the kept page goes now, whatever the start does
  }
  log().info << "call again call_id=" << page.call_id << " video=" << (video ? 1 : 0);
  // StartCall leaves the failed call first (GuardNewCall), on the same owner queue as the new call.
  (void)StartCall(page.thread_id, video, WhileAlive([this](const CallStartResult& result) {
                    redial_peer_.clear(); // its start answered
                    if (result.kind != CallStartResult::Kind::Started && RedialPending()) {
                      log().info << "call again did not start";
                      redial_pending_since_ms_ = 0;
                    }
                  }));
  if (live_page && RedialPending()) {
    // The new call's start took the failed call off the "being left" latch (RequestStart): until the owner has run
    // its leave, the failed call is still on record and would come back on screen, its buttons with it.
    StartLeaving(page.call_id);
  }
}

bool CallPresenter::RingingFrom(const std::string& identity) const {
  auto* backend = Backend();
  if (identity.empty() || !backend || !backend->Available()) {
    return false;
  }
  auto ring = backend->TopPendingInvite();
  return ring && ring->has_value() && (*ring)->inviter_identity == identity;
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
    if (auto allowed = backend->VideoAllowedForCall(active_call_id_); !allowed || !allowed->has_value() || !**allowed) {
      Fail("call.error.video_not_allowed");
      return;
    }
  }
  backend->SetLocalVideoEnabled(next, WhileAlive([this](Roe<void> cam) {
                                  if (!cam) {
                                    // The backend's reason is English technical text: the log keeps it, the user sees a plain sentence.
                                    log().info << "camera toggle failed: " << cam.error().message;
                                    Fail("call.error.camera_failed");
                                  }
                                  Refresh();
                                }));
}

void CallPresenter::FlipCamera() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available() || !backend->Media().IsActive()) {
    return;
  }
  // The button shows only while the camera is on; a tap that lost the race with turning it off is
  // logged, not shown.
  if (auto flipped = backend->Media().FlipCamera(); !flipped) {
    log().info << "FlipCamera refused: " << flipped.error().message;
  }
  Refresh();
}

void CallPresenter::ReportCameraFailure(const CameraFailure& failure) {
  const char* key = "call.error.camera_failed";
  switch (failure.kind) {
    case CameraFailure::Kind::Busy:
      key = "call.error.camera_busy";
      break;
    case CameraFailure::Kind::NotResponding:
      key = "call.error.camera_not_responding";
      break;
    case CameraFailure::Kind::PermissionDenied:
      key = "call.error.camera_denied";
      break;
    case CameraFailure::Kind::Failed:
      break;
  }
  log().info << "camera failure shown as " << key << " raw=" << failure.detail;
  Fail(key);
}

void CallPresenter::ToggleSpeaker() {
  // Any time a call is on screen — ringing out, connecting, in the call — not only once media runs: the
  // session routes it now if call audio is up (ringback or media), else when media takes the session.
  if (!AudioSession::SupportsSpeakerToggle() || screen_.kind == CallScreen::Kind::None) {
    return;
  }
  BindToMessaging();
  auto* backend = Backend();
  const bool on = !SpeakerShownOn();
  // Speaker = route only (not mute). Android: with media up the route changes around the mic — the engine closes it,
  // the route changes, it reopens — or the OS disconnects the open recording stream and SDL never gives it back.
  const bool reopen =
      AudioSession::SpeakerToggleNeedsDeviceReopen() && backend && backend->Available() && backend->Media().IsActive();
  const CallSpeakerTapPlan plan =
      PlanCallSpeakerTap(reopen, reopen && dev_route_reorder_skip_ && dev_route_reorder_skip_());
  if (plan.around_the_mic) {
    speaker_tap_ = CallSpeakerTap{.on = on, .at_ms = util::NowUnixMs()};
    backend->Media().RequestAudioRouteChange([on]() { AudioSession::SetSpeakerphoneOn(on); });
  } else {
    speaker_tap_.reset();
    AudioSession::SetSpeakerphoneOn(on);
    if (plan.reopen_after) {
      backend->Media().RequestAudioDeviceReopen();
    }
  }
  log().info << "ToggleSpeaker speaker_on=" << (on ? 1 : 0) << " reopen=" << (reopen ? 1 : 0)
             << " mic_first=" << (plan.around_the_mic ? 1 : 0);
  Refresh();
}

bool CallPresenter::SpeakerShownOn() const {
  auto* backend = Backend();
  const bool engine = speaker_tap_ && backend && backend->Available();
  const IMediaView::AudioRouteChange state =
      engine ? backend->Media().AudioRouteChangeState() : IMediaView::AudioRouteChange::None;
  const bool reopening = engine && backend->Media().AudioDevicesReopening();
  return CallSpeakerButtonOn(speaker_tap_, state, reopening, util::NowUnixMs(), AudioSession::IsSpeakerphoneOn());
}

void CallPresenter::SetDevRouteReorderSkip(std::function<bool()> skip) {
  dev_route_reorder_skip_ = std::move(skip);
}

} // namespace pbr
