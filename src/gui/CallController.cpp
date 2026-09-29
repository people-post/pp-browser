#include <stdexcept>
#include "gui/CallController.h"

#include "foundation/i18n/LocalizationService.h"
#include "domain/media/CallAudioSession.h"
#include "domain/media/CallMediaEngine.h"
#include "domain/media/CameraCaptureOrientation.h"
#include "domain/media/DeviceVitals.h"
#include "common/media/CallMediaHealth.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/CallHopAttachLogic.h"
#include "domain/people/AvatarGlyph.h"
#include "domain/people/ContactTypes.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/PlatformUserHints.h"
#include "foundation/runtime/ProductBranding.h"
#include "domain/ui/ShellTypes.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "feature/calls/CallLifecycle.h"
#include "feature/calls/CallUiBackend.h"
#include "gui/CallChromeSync.h"
#include "gui/CallMetricsTracker.h"
#include "domain/ui/CallConflictCopy.h"
#include "domain/ui/PaymentFeedback.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "CallVideoTileRenderer.h"
#include "gui/UserFeedback.h"

#include "foundation/data/PricingTypes.h"

#include "common/Utilities.h"

#include <ui/Core.h>
#include <ui/base/SystemInterface.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

CallController::CallController() {
  redirectLogger("CallController");
}

namespace {

/** Remote frames this fresh mean the peer's camera is on, whatever its roster row says (B58). */
constexpr int64_t kPeerVideoLiveMs = 500;
/** A video call shows "Starting video…" at most this long while it waits for the first frame. */
constexpr int64_t kVideoStartMs = 10'000;

CallChromeLayer CaptureCallChrome(const CallRingState& ring, const CallInProgressState& in_call) {
  return {
      .ring_active = ring.active,
      .in_call_active = in_call.active,
      .ring_pulse = ring.pulse,
      .in_call_muted = in_call.muted,
      .in_call_camera_on = in_call.camera_on,
      .in_call_speaker_on = in_call.speaker_on,
      .in_call_stage_visible = in_call.stage_visible,
      .in_call_remote_video = in_call.remote_video,
      .in_call_local_preview = in_call.local_preview,
      .ring_conflict = ring.conflict,
      .ring_show_pricing = ring.show_pricing,
      .ring_accept_charge_enabled = ring.accept_charge_enabled,
      .ring_call_id = ring.call_id.c_str(),
      .in_call_id = in_call.call_id.c_str(),
      .in_call_subtitle = in_call.subtitle.c_str(),
      .ring_caller_label = ring.caller_label.c_str(),
      .ring_media_label = ring.media_label.c_str(),
      .ring_eyebrow = ring.eyebrow.c_str(),
      .ring_conflict_hint = ring.conflict_hint.c_str(),
      .ring_accept_label = ring.accept_label.c_str(),
      .ring_voice_answer_label = ring.voice_answer_label.c_str(),
      .ring_decline_label = ring.decline_label.c_str(),
      .ring_pricing_label = ring.pricing_label.c_str(),
      .ring_accept_charge_label = ring.accept_charge_label.c_str(),
      .ring_accept_charge_hint = ring.accept_charge_hint.c_str(),
      .ring_accept_short = ring.accept_short.c_str(),
      .ring_voice_short = ring.voice_short.c_str(),
      .in_call_title = in_call.title.c_str(),
      .in_call_mic_level = in_call.mic_level,
      .in_call_peer_level = in_call.peer_level,
      .in_call_mic_hint = in_call.mic_hint.c_str(),
      .in_call_peer_hint = in_call.peer_hint.c_str(),
      .in_call_elapsed = in_call.elapsed.c_str(),
      .in_call_peer_label = in_call.peer_label.c_str(),
      .in_call_remote_placeholder = in_call.remote_placeholder.c_str(),
      .in_call_show_roster = in_call.show_roster,
      .in_call_show_invite = in_call.show_invite,
      .in_call_show_retry = in_call.show_retry,
      .in_call_show_speaker = in_call.show_speaker,
      .in_call_show_camera = in_call.show_camera,
      .in_call_participant_count = in_call.participant_count,
      .in_call_status_hint = in_call.status_hint.c_str(),
      .in_call_mode = in_call.mode,
      .in_call_minimized_corner = in_call.minimized_corner,
      .in_call_quality_bars = in_call.quality_bars,
      .in_call_quality_ok = in_call.quality_ok,
      .in_call_quality_warn = in_call.quality_warn,
      .in_call_quality_error = in_call.quality_error,
      .in_call_quality_label = in_call.quality_label.c_str(),
      .in_call_quality_hint = in_call.quality_hint.c_str(),
      .in_call_show_debug_subtitle = in_call.show_debug_subtitle,
      .in_call_debug_subtitle = in_call.debug_subtitle.c_str(),
  };
}

int QuantizeAudioLevel(float level) {
  if (level < 0.008f) {
    return 0;
  }
  if (level < 0.03f) {
    return 1;
  }
  if (level < 0.08f) {
    return 2;
  }
  if (level < 0.18f) {
    return 3;
  }
  if (level < 0.35f) {
    return 4;
  }
  return 5;
}

std::string LevelHint(int level, bool remote, bool muted) {
  if (muted && !remote) {
    return Tr("call.level.muted");
  }
  if (level <= 0) {
    return remote ? Tr("call.level.quiet") : Tr("call.level.silent");
  }
  if (level <= 2) {
    return Tr("call.level.speaking");
  }
  return Tr("call.level.loud");
}

std::string ComposeP2pStatusHint(bool missing_mic) {
  const std::map<std::string, std::string> product{{"product", kProductName}};
  std::string hint = Tr(PlatformUserHints::P2pNetworkHintKey(), product);
  if (missing_mic) {
    const std::string mic = Tr(PlatformUserHints::MicBlockedHintKey());
    if (!mic.empty()) {
      if (!hint.empty()) {
        hint += " ";
      }
      hint += mic;
    }
  }
  return hint;
}

std::string ComposeGroupCallStatusHint() {
  return Tr("call.hint.group_media");
}

} // namespace

void CallController::BindCallPorts(CallFunctionalPorts ports) {
  call_ports_ = std::move(ports);
  BindToMessaging();
}

void CallController::BindShellCallChrome(ShellCallChromePorts ports) {
  shell_call_chrome_ = std::move(ports);
}

void CallController::BindPeerLinkRefresh(std::function<void()> callback) {
  peer_link_refresh_ = std::move(callback);
}

void CallController::BindPeoplePickerNotify(PeoplePickerNotifyPorts ports) {
  people_picker_notify_ = std::move(ports);
}

bool CallController::MessagingInitialized() const {
  return call_ports_.initialized && call_ports_.initialized();
}

CallUiBackend* CallController::Backend() {
  return call_ports_.backend ? call_ports_.backend() : nullptr;
}

void CallController::BindToMessaging() {
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
  backend->SetOnRingChanged([this]() {
    // Ingest may run on IO; shell/RmlUi updates must stay on UI.
    AppRuntime::PostUI([this]() {
      RefreshPendingRing();
      if (peer_link_refresh_) {
        peer_link_refresh_();
      }
    });
  });
  backend->SetOnChromeRefresh([this]() {
    RefreshPendingRing();
    if (peer_link_refresh_) {
      peer_link_refresh_();
    }
  });
  bound_calls_ = identity;
  // Pick up post-restart abandon / pending ring after stack rebuild.
  RefreshPendingRing();
}

void CallController::Tick() {
  BindToMessaging();
  if (auto* backend = Backend()) {
    backend->SweepExpiredInvites();
  }
  const int64_t now = util::NowUnixMs();
  if (ring_.active) {
    if (now - last_pulse_toggle_ms_ >= 600) {
      ring_.pulse = !ring_.pulse;
      last_pulse_toggle_ms_ = now;
      SyncShellState();
    }
  }
  RefreshCallLevels();
  SyncRingtone();
  if (metrics_.Tracking()) {
    auto* backend = Backend();
    const bool call_live = ring_.active || !active_call_id_.empty() ||
                           (backend && backend->Available() && backend->Phase() != CallPhase::Idle);
    EmitMetrics(metrics_.Tick(call_live, now, Vitals(now)));
  }
  // UI-stuck probe only while a call is tracked (a thread posting a ping every 500 ms).
  if (metrics_.Tracking() && !ui_probe_.Running()) {
    ui_probe_.Start([](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); },
                    [this](int64_t latency_ms) { EmitMetrics(metrics_.NoteUiLatency(latency_ms)); });
  } else if (!metrics_.Tracking() && ui_probe_.Running()) {
    ui_probe_.Stop();
  }
}

const DeviceVitals& CallController::Vitals(const int64_t now_ms) {
  // UIDevice / getrusage: every couple of seconds is plenty for power and thermal trends.
  if (vitals_read_ms_ == 0 || now_ms - vitals_read_ms_ >= 2000) {
    vitals_ = ReadDeviceVitals();
    vitals_read_ms_ = now_ms;
  }
  return vitals_;
}

void CallController::EmitMetrics(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    MetricsLog().info << line;
  }
}

void CallController::OnCallWake() {
  // Inbox sync is async; notify once RefreshPendingRing sees the invite.
  // Coordinator / wake sync must not mutate shell chrome here (CALLS.md thread policy).
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { OnCallWake(); });
    return;
  }
  pending_call_wake_notify_ = true;
  RefreshPendingRing();
}

void CallController::ClearRing() {
  ringing_call_id_.clear();
  ring_started_ms_ = 0;
  ring_ = {};
  ringtone_.Stop();
}

void CallController::PrepareForShutdown() {
  if (!AppRuntime::CurrentlyOnUI()) {
    log().warning << "PrepareForShutdown off UI thread";
  }
  ringing_call_id_.clear();
  ring_started_ms_ = 0;
  ring_ = {};
  ui_probe_.Stop();
  // Budgeted join before SDL_Quit — do not hang product quit on SDL device close.
  if (!ringtone_.StopAndJoin(CallRingtone::kDefaultShutdownJoinBudget)) {
    log().warning << "PrepareForShutdown: ringtone join budget exceeded — detached";
  }
  if (!ringback_.StopAndJoin(CallRingtone::kDefaultShutdownJoinBudget)) {
    log().warning << "PrepareForShutdown: ringback join budget exceeded — detached";
  }
}

void CallController::ClearInCall() {
  active_call_id_.clear();
  chrome_mode_ = CallChromeMode::Expanded;
  restore_mode_ = CallChromeMode::Expanded;
  minimized_corner_ = 0;
  chrome_mode_call_id_.clear();
  last_media_health_log_ms_ = 0;
  audio_fault_gate_.Reset();
  peer_video_log_key_.clear();
  video_start_since_ms_ = 0;
  camera_sync_off_done_ = false;
  // Not auto_camera_pending_ / auto_camera_call_id_: the callee's accept is async, and Tick runs
  // this for the "no active local call yet" frames between the Accept click and the session going
  // Active — clearing here dropped the callee's auto camera intermittently (device test
  // 2026-09-28). The intent is keyed by call_id, so a stale one can never fire for another call.
  in_call_ = {};
  CallVideoTileRenderer::Instance().Clear();
}

void CallController::HideInCallChrome() {
  in_call_ = {};
  CallVideoTileRenderer::Instance().Clear();
}

void CallController::ApplyChromeModeToState(CallInProgressState& in_call) {
  in_call.mode = chrome_mode_;
  in_call.mode_str = CallChromeModeName(chrome_mode_);
  in_call.minimized_corner = minimized_corner_;
}

void CallController::SetChromeMode(CallChromeMode mode) {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this, mode]() { SetChromeMode(mode); });
    return;
  }
  if (mode == CallChromeMode::Minimized) {
    if (chrome_mode_ != CallChromeMode::Minimized) {
      restore_mode_ = chrome_mode_ == CallChromeMode::Immersive ? CallChromeMode::Immersive
                                                               : CallChromeMode::Expanded;
    }
  } else {
    restore_mode_ = mode;
  }
  if (chrome_mode_ == mode) {
    return;
  }
  chrome_mode_ = mode;
  if (in_call_.active) {
    chrome_mode_call_id_ = in_call_.call_id.c_str();
    ApplyChromeModeToState(in_call_);
    SyncShellState();
  }
}

void CallController::MinimizeChrome() {
  SetChromeMode(CallChromeMode::Minimized);
}

void CallController::ExpandChrome() {
  SetChromeMode(CallChromeMode::Expanded);
}

void CallController::ImmersiveChrome() {
  SetChromeMode(CallChromeMode::Immersive);
}

void CallController::RestoreChromeFromMinimized() {
  SetChromeMode(restore_mode_ == CallChromeMode::Immersive ? CallChromeMode::Immersive
                                                          : CallChromeMode::Expanded);
}

void CallController::SetMinimizedCorner(int corner) {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this, corner]() { SetMinimizedCorner(corner); });
    return;
  }
  const int clamped = std::clamp(corner, 0, 3);
  if (minimized_corner_ == clamped) {
    return;
  }
  minimized_corner_ = clamped;
  if (in_call_.active) {
    in_call_.minimized_corner = minimized_corner_;
    SyncShellState();
  }
}

void CallController::SyncShellState() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { SyncShellState(); });
    return;
  }
  if (!shell_call_chrome_.apply_snapshot) {
    return;
  }
  const CallChromeLayer next = CaptureCallChrome(ring_, in_call_);
  const CallChromeUpdate update = ClassifyCallChromeUpdate(synced_chrome_, next);
  synced_chrome_ = next;

  if (update == CallChromeUpdate::None) {
    return;
  }
  // ShellHost copies snapshot then remount / DirtyCallChrome / force-frame.
  shell_call_chrome_.apply_snapshot({ring_, in_call_}, update);
}

void CallController::SyncRingtone() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { SyncRingtone(); });
    return;
  }
  // CallRingtone::Stop is async (never joins on Accept/UI) so mobile is safe — see
  // CallRingtone.cpp Samsung Accept hang notes. Do not skip playback here.
  const bool should_ring = ring_.active;
  if (should_ring && !ringtone_.IsPlaying()) {
    ringtone_.Start();
  } else if (!should_ring && ringtone_.IsPlaying()) {
    ringtone_.Stop();
  }

  // Caller-side ringback: only while the callee has not answered (OutboundCalling) and
  // no incoming ring is showing and media has not started yet.
  auto* backend = Backend();
  const bool should_ringback = !should_ring && backend && backend->Available() &&
                                backend->Phase() == CallPhase::OutboundCalling &&
                                !backend->Media().IsActive();
  if (should_ringback && !ringback_.IsPlaying()) {
    ringback_.Start();
  } else if (!should_ringback && ringback_.IsPlaying()) {
    StopRingback(backend);
  }
}

void CallController::StopRingback(CallUiBackend* backend) {
  const bool ringback_was_playing = ringback_.IsPlaying();
  if (!ringback_was_playing) {
    return;
  }
  // The ringback worker itself owns activate/release of the audio session (see
  // CallRingtone::RunLoop) — a fast cancel could otherwise race a Deactivate() here
  // against the worker's own ActivateForVoipCall() and leave the phone in VoIP mode.
  // The engine owns the session once media is active — release unless the call engine
  // is actually active and owns it (a null/unavailable backend means nobody else does).
  const bool release = !(backend && backend->Available() && backend->Media().IsActive());
  ringback_.SetReleaseSessionOnStop(release);
  ringback_.Stop();
}

std::string CallController::DisplayNameForIdentity(const std::string& identity) const {
  if (identity.empty() || !call_ports_.find_contact_by_identity) {
    return {};
  }
  if (auto contact = call_ports_.find_contact_by_identity(identity, ContactIdKind::Account)) {
    if (*contact) {
      std::string name =
          (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (!name.empty()) {
        return name;
      }
    }
  }
  return identity;
}

std::string CallController::FormatElapsed(const int64_t connected_at_ms) {
  if (connected_at_ms <= 0) {
    return {};
  }
  const int64_t sec = std::max<int64_t>(0, (util::NowUnixMs() - connected_at_ms) / 1000);
  const int64_t mins = sec / 60;
  const int64_t secs = sec % 60;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(mins), static_cast<long long>(secs));
  return buf;
}

void CallController::RefreshPendingRing() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { RefreshPendingRing(); });
    return;
  }
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }

  if (!backend->LastError().empty()) {
    UserFeedback::Fail(PaymentErrorUserMessage(backend->LastError()));
    backend->ClearLastError();
  }

  if (auto media_err = backend->TakeLastMediaError(); media_err && !media_err->empty()) {
    // SoftMigrate / hop failures need a sticky banner — toast (even Long=6s) vanishes before
    // users can read multi-hop diagnostics.
    UserFeedback::NeedsSetup(PaymentErrorUserMessage(*media_err));
  }
  // The peer ended a call we were in (also a failed one — failed is not closed): the panel is
  // about to go, so say why instead of letting the call silently vanish.
  if (auto ended = backend->TakeRemoteEnd(); ended) {
    std::string name;
    if (auto peer = backend->PeerIdentityForCall(ended->call_id); peer && peer->has_value()) {
      name = DisplayNameForIdentity(**peer);
    }
    const char* named = ended->declined ? "call.status.declined_by_peer" : "call.status.ended_by_peer";
    const char* unknown =
        ended->declined ? "call.status.declined_by_peer_unknown" : "call.status.ended_by_peer_unknown";
    UserFeedback::Ok(name.empty() ? Tr(unknown) : Tr(named, {{"name", name}}));
  }

  // Attach-wait / connect health are SM-owned timers (V039 pm3) — do not poll on UI tick.

  auto top = backend->TopPendingInvite();
  if (top && top->has_value()) {
    const bool accept_in_flight = backend->ShouldSuppressRing((*top)->call_id);
    auto active = backend->ActiveLocalCall();
    const bool same_call_active =
        active && active->has_value() && (*active)->call_id == (*top)->call_id;

    // Accept in flight (CALLS.md): dismiss ring immediately; show Connecting bar so Accept
    // does not look hung while AcceptInvite runs on the worker.
    if (accept_in_flight && !same_call_active) {
      ringing_call_id_ = (*top)->call_id;
      ClearRing();
      {
        in_call_.active = true;
        in_call_.call_id = (*top)->call_id;
        in_call_.subtitle = Tr("call.status.connecting").c_str();
        in_call_.title = (*top)->video_allowed ? Tr("call.title.video").c_str()
                                               : Tr("call.title.voice").c_str();
        const std::string caller = DisplayNameForIdentity((*top)->inviter_identity);
        in_call_.peer_label = caller.empty() ? (*top)->inviter_identity.c_str() : caller.c_str();
        // Answered with video: switch to the video stage right away (see kVideoStartMs).
        const bool answered_video = auto_camera_pending_ && auto_camera_call_id_ == (*top)->call_id;
        in_call_.stage_visible = answered_video;
        in_call_.remote_video = false;
        in_call_.remote_placeholder = answered_video ? Tr("call.placeholder.starting_video").c_str() : "";
      }
      SyncShellState();
      SyncRingtone();
      return;
    }

    // Same-call pending (duplicate invite / inbox race): keep in-call chrome; do not flip to ring.
    if (same_call_active) {
      // Fall through to in-call rendering below.
    } else {
      ringing_call_id_ = (*top)->call_id;
      if (backend->Phase() == CallPhase::Idle) {
        backend->Apply(CallLifecycleEvent::InviteSeen, ringing_call_id_);
      }
      if (ring_started_ms_ == 0) {
        ring_started_ms_ = util::NowUnixMs();
      }

      const std::string caller_label = [&]() {
        std::string name = DisplayNameForIdentity((*top)->inviter_identity);
        if (name.empty()) {
          name = (*top)->inviter_identity;
        }
        return name;
      }();

      bool has_conflict = false;
      bool same_peer = false;
      std::string active_peer_label;
      if (active && active->has_value() && (*active)->call_id != (*top)->call_id) {
        has_conflict = true;
        active_call_id_ = (*active)->call_id;
        if (auto peer = backend->PeerIdentityForCall((*active)->call_id); peer && peer->has_value()) {
          active_peer_label = DisplayNameForIdentity(**peer);
          if (active_peer_label.empty()) {
            active_peer_label = **peer;
          }
          same_peer = (**peer == (*top)->inviter_identity);
        }
      }
      // Do not clear active_call_id_ here — only Leave / no-active paths should.

      const CallConflictCopy copy =
          MakeCallConflictCopy(has_conflict, same_peer, caller_label, active_peer_label);

      // Hide in-call bar while ringing a *different* call; keep active_call_id_ when conflicting.
      HideInCallChrome();

      const bool was_active = ring_.active;
      ring_.active = true;
      ring_.conflict = has_conflict;
      ring_.call_id = (*top)->call_id;
      ring_.caller_label = caller_label;
      ring_.video_allowed = (*top)->video_allowed;
      // M6: headline just says what kind of call this is; the voice-answer button covers the choice.
      ring_.media_label = (*top)->video_allowed ? Tr("call.ring.incoming_video").c_str()
                                                : Tr("call.ring.incoming_voice").c_str();
      if (!was_active) {
        log().warning
            << "RefreshPendingRing activate call_id=" << ringing_call_id_;
        const int64_t now = util::NowUnixMs();
        EmitMetrics(metrics_.NoteRing(ringing_call_id_, (*top)->video_allowed, now, Vitals(now)));
      }
      ring_.eyebrow = copy.eyebrow;
      ring_.conflict_hint = copy.hint;
      ring_.accept_label = copy.accept_label;
      ring_.decline_label = copy.decline_label;
      // V035: video ring offers a voice-answer alternative; the primary button reads
      // "Video answer" so it's clear Accept keeps the camera option (conflict wording unchanged).
      if (ring_.video_allowed && !has_conflict) {
        ring_.accept_label = Tr("call.ring.accept_video").c_str();
      }

      // P001: show waive / take-all when inviter offered a positive initiation amount.
      const int64_t offer_minor = backend->InitiationOfferMinorForPeer((*top)->inviter_identity);
      ring_.show_pricing = offer_minor > 0;
      ring_.accept_charge_enabled = offer_minor > 0 && PaymentRailsAvailable();
      if (ring_.show_pricing) {
        const std::string amount = std::to_string(offer_minor);
        ring_.pricing_label =
            Tr("call.pricing.offer", {{"amount", amount}, {"currency", kPricingCurrencyDisplayName}})
                .c_str();
        ring_.accept_charge_label =
            Tr("call.ring.accept_charge", {{"amount", amount}, {"currency", kPricingCurrencyDisplayName}})
                .c_str();
        ring_.accept_charge_hint = Tr("call.ring.accept_charge_disabled_hint").c_str();
        if (!has_conflict) {
          ring_.accept_label = Tr("call.ring.accept_free").c_str();
        } else {
          ring_.accept_label = Tr("call.ring.end_and_accept_free").c_str();
        }
      } else {
        ring_.pricing_label.clear();
        ring_.accept_charge_label.clear();
        ring_.accept_charge_hint.clear();
      }

      // I2/M3: voice-answer button label — conflict wording wins over pricing when both apply.
      if (has_conflict) {
        ring_.voice_answer_label = Tr("call.ring.end_and_accept_voice").c_str();
      } else if (ring_.show_pricing) {
        ring_.voice_answer_label = Tr("call.ring.voice_answer_free").c_str();
      } else {
        ring_.voice_answer_label = Tr("call.ring.accept_voice").c_str();
      }

      // Short labels for the round icon buttons (WeChat-style ring redesign) — same
      // conflict-wins-over-pricing precedence as the long labels above.
      if (has_conflict) {
        ring_.accept_short = Tr("call.ring.short.end_and_accept").c_str();
        ring_.voice_short = Tr("call.ring.short.end_and_accept_voice").c_str();
      } else if (ring_.show_pricing) {
        ring_.accept_short = Tr("call.ring.short.accept_free").c_str();
        ring_.voice_short = Tr("call.ring.short.voice_answer_free").c_str();
      } else if (ring_.video_allowed) {
        ring_.accept_short = Tr("call.ring.short.video").c_str();
        ring_.voice_short = Tr("call.ring.short.voice").c_str();
      } else {
        ring_.accept_short = Tr("call.ring.short.accept").c_str();
        ring_.voice_short = Tr("call.ring.short.voice").c_str();
      }

      if (pending_call_wake_notify_) {
        pending_call_wake_notify_ = false;
        const std::string body =
            caller_label.empty()
                ? Tr("call.notify.body_unknown")
                : Tr("call.notify.body_named", {{"name", caller_label}});
        ILocalNotifier::Instance().NotifyIncoming(Tr("call.notify.title"), body, "");
      }
      SyncShellState();
      SyncRingtone();
      return;
    }
  }

  if (auto active = backend->ActiveLocalCall(); active && active->has_value()) {
    // LeaveClicked sets Idle before LeaveCall IO finishes — do not resurrect the panel from the
    // still-Active disk row (Samsung: End looked hung / "couldn't connect" stuck).
    if (backend->Phase() == CallPhase::Idle) {
      if ((*active)->state == CallSessionState::Active && !backend->Media().IsActive() &&
          !backend->MediaAttemptedThisProcess((*active)->call_id) && !backend->IsAwaitingSfuRecovery()) {
        // True orphan after force-quit / process restart.
        backend->LeaveCall((*active)->call_id, LiveCallEndReason::Orphaned);
      }
      active_call_id_.clear();
      ClearInCall();
      ClearRing();
      SyncShellState();
      return;
    }

    active_call_id_ = (*active)->call_id;
    ClearRing();

    // Outbound unanswered TTL → Leave lives in CallSessionManager::SweepExpiredInvites
    // (CALLS design; Tick already sweeps). Do not duplicate LeaveClicked here.

    // Direct connect failed: keep chrome for Retry/End on 1:1. Group SFU recovery keeps chrome too.
    // Do not auto-LeaveCall on `failed` — that erased the session before the user could retry.
    if (backend->Media().IsActive() && backend->Media().ActiveCallId() == active_call_id_) {
      const std::string media_state = backend->Media().ConnectionState();
      if (media_state == "failed" && !backend->IsAwaitingSfuRecovery() && !backend->Media().IsSfuMode() &&
          !backend->IsP2pConnectFailed()) {
        // State callback may not have marked yet (ordering); ensure UI can show Retry.
        backend->PollP2pConnectHealth();
      }
    }

    auto& in_call = in_call_;
    in_call.active = true;
    in_call.call_id = (*active)->call_id;
    in_call.muted = backend->Media().IsMuted();
    in_call.show_speaker = CallAudioSession::SupportsSpeakerToggle();
    in_call.speaker_on = CallAudioSession::IsSpeakerphoneOn();

    const bool is_video = (*active)->video_allowed;
    int joined_count = 0;
    std::string local_identity;
    if (call_ports_.local_relay_identity) {
      local_identity = call_ports_.local_relay_identity().value_or(std::string{});
    }
    if (auto participants = backend->ListJoinedParticipants((*active)->call_id); participants) {
      joined_count = static_cast<int>(participants->size());
      in_call.participant_count = joined_count;
      in_call.show_roster = joined_count > 2 || (*active)->origin_group_id.has_value();
      in_call.show_invite = true;
      in_call.roster.clear();
      in_call.roster.reserve(participants->size());
      for (const CallParticipant& row : *participants) {
        CallRosterParticipantState entry;
        entry.is_local = !local_identity.empty() && row.identity == local_identity;
        std::string name = entry.is_local ? Tr("call.label.you") : DisplayNameForIdentity(row.identity);
        if (!entry.is_local && name.empty()) {
          name = row.identity;
        }
        entry.name = name.c_str();
        entry.identity = row.identity.c_str();
        if (call_ports_.identity_icon_local_path) {
          const std::string avatar_path = call_ports_.identity_icon_local_path(row.identity);
          if (!avatar_path.empty()) {
            entry.has_avatar = true;
            entry.avatar_src = avatar_path.c_str();
          }
        }
        {
          const AvatarGlyph glyph = MakeAvatarGlyph(name, row.identity);
          entry.avatar_letter = glyph.letter.c_str();
          entry.avatar_tone = glyph.tone;
        }
        entry.stream_id = std::to_string(PublisherStreamIdForIdentity(row.identity)).c_str();
        entry.audio_muted = row.media.audio_muted;
        entry.video_enabled = row.media.video_enabled;
        in_call.roster.push_back(std::move(entry));
      }
    } else {
      in_call.participant_count = 0;
      in_call.show_roster = false;
      in_call.show_invite = true;
      in_call.roster.clear();
    }

    // V031: pick default once per call; preserve user mode across ticks.
    if (chrome_mode_call_id_ != (*active)->call_id) {
      chrome_mode_ = DefaultCallChromeMode(in_call.show_roster);
      restore_mode_ = chrome_mode_;
      chrome_mode_call_id_ = (*active)->call_id;
    }
    ApplyChromeModeToState(in_call);

    if (in_call.show_roster) {
      in_call.title = is_video ? Tr("call.title.group_video").c_str() : Tr("call.title.group_voice").c_str();
    } else {
      in_call.title = is_video ? Tr("call.title.video").c_str() : Tr("call.title.voice").c_str();
    }

    std::string peer_label = Tr("call.label.them");
    if (auto peer = backend->PeerIdentityForCall((*active)->call_id); peer && peer->has_value()) {
      const std::string name = DisplayNameForIdentity(**peer);
      if (!name.empty()) {
        peer_label = name;
      }
    }
    in_call.peer_label = in_call.show_roster ? Tr("call.label.others").c_str() : peer_label.c_str();

    const bool mesh_messaging_failed = backend->IsP2pConnectFailed();
    const bool group_call_context = (*active)->origin_group_id.has_value() || joined_count > 2 ||
                                    backend->IsAwaitingSfuRecovery() || backend->Media().IsSfuMode();
    in_call.show_retry = mesh_messaging_failed && !backend->IsAwaitingSfuRecovery() && !backend->Media().IsSfuMode();

    // V037: Connected when Lifecycle MediaChromeLive (InCall + DirectLive|HopLive).
    // Seat Live remains a fallback while Status catches up; activity still wins in flight.
    const auto seat_media = backend->SeatMediaState(active_call_id_);
    const bool media_live =
        backend->MediaChromeLive() || seat_media == CallMediaSeat::MediaState::Live;
    const bool media_connecting =
        backend->MediaStatus() == CallMediaStatus::DirectConnecting ||
        backend->MediaStatus() == CallMediaStatus::HopAttaching ||
        backend->MediaStatus() == CallMediaStatus::HopWaiting ||
        backend->MediaStatus() == CallMediaStatus::Migrating ||
        seat_media == CallMediaSeat::MediaState::Connecting;
    const bool media_connected = media_live;
    std::string activity = backend->PeekMediaActivity();
    if (!activity.empty() && media_connected && !backend->IsAwaitingSfuRecovery()) {
      backend->ClearMediaActivity();
      activity.clear();
    }
    if (mesh_messaging_failed) {
      in_call.elapsed = {};
      in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
      in_call.status_hint =
          group_call_context ? ComposeGroupCallStatusHint().c_str()
                             : ComposeP2pStatusHint(backend->P2pConnectMissingMic()).c_str();
      in_call.show_invite = false;
    } else if (!activity.empty()) {
      in_call.elapsed = {};
      in_call.subtitle = activity.c_str();
      in_call.status_hint = {};
      if (activity == Tr("call.status.switching_media_path")) {
        in_call.status_hint = Tr("call.hint.switching_media_path").c_str();
      } else if (activity == Tr("call.status.looking_for_another_path")) {
        in_call.status_hint = Tr("call.hint.looking_for_another_path").c_str();
      }
    } else if (backend->MediaStatus() == CallMediaStatus::Reconnecting) {
      // k4: the call lost its path and waits for a new one; the call timer keeps running.
      in_call.elapsed = FormatElapsed(backend->Media().ConnectedAtMs());
      const std::string reconnecting = Tr("call.status.reconnecting");
      in_call.subtitle =
          in_call.elapsed.empty() ? reconnecting.c_str() : (std::string(in_call.elapsed.c_str()) + " · " + reconnecting).c_str();
      in_call.status_hint = {};
    } else if (backend->MediaStatus() == CallMediaStatus::DegradedTxOnly) {
      in_call.elapsed = {};
      in_call.subtitle = Tr("call.quality.hint.sending_only").c_str();
      in_call.status_hint = {};
    } else if (media_connected) {
      in_call.elapsed = FormatElapsed(backend->Media().ConnectedAtMs());
      in_call.subtitle = in_call.elapsed.empty() ? Tr("call.status.connected").c_str() : in_call.elapsed;
      in_call.show_retry = false;
      in_call.status_hint = {};
    } else {
      in_call.elapsed = {};
      in_call.status_hint = {};
      if (seat_media == CallMediaSeat::MediaState::Failed ||
          backend->MediaStatus() == CallMediaStatus::Failed) {
        in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
      } else if (seat_media == CallMediaSeat::MediaState::Idle && !backend->Media().IsActive() &&
                 backend->MediaStatus() == CallMediaStatus::None) {
        in_call.subtitle = Tr("call.status.calling").c_str();
      } else if (backend->IsSoftMigrateInFlight() || backend->IsSfuAttachWaitActive() ||
                 media_connecting) {
        // V037: Status Migrating / HopWaiting own chrome; soft_migrate_in_flight_ is latch only.
        if (backend->MediaStatus() == CallMediaStatus::Migrating ||
            (backend->MediaStatus() != CallMediaStatus::HopWaiting &&
             backend->MediaStatus() != CallMediaStatus::HopAttaching &&
             backend->IsSoftMigrateInFlight())) {
          in_call.subtitle = Tr("call.status.setting_up_group").c_str();
        } else if (backend->MediaStatus() == CallMediaStatus::HopWaiting ||
                   backend->IsSfuAttachWaitActive()) {
          in_call.subtitle = Tr("call.status.waiting_for_media_path").c_str();
        } else {
          in_call.subtitle = Tr("call.status.connecting").c_str();
        }
      } else if (!backend->Media().IsActive()) {
        in_call.subtitle = Tr("call.status.calling").c_str();
      } else {
        const std::string state = backend->Media().ConnectionState();
        if (backend->IsAwaitingSfuRecovery()) {
          in_call.subtitle = Tr("call.status.reconnecting").c_str();
        } else if (state == "connecting" || state.empty() || state == "new") {
          in_call.subtitle = Tr("call.status.connecting").c_str();
        } else if (state == "disconnected") {
          in_call.subtitle = Tr("call.status.reconnecting").c_str();
        } else if (state == "failed") {
          in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
        } else if (state == "closed") {
          in_call.subtitle = Tr("call.status.connecting").c_str();
        } else {
          in_call.subtitle = state;
        }
      }
    }
    if (in_call.show_roster && joined_count > 0 && media_connected && activity.empty()) {
      in_call.subtitle =
          Tr("call.participants_elapsed",
             {{"count", std::to_string(joined_count)}, {"elapsed", std::string(in_call.elapsed.c_str())}})
              .c_str();
    }
    // 2026-09-28 product decision: video calls start with the local camera on (supersedes V009's
    // "join with camera off" default). auto_camera_pending_ is set once by the caller
    // (StartCallWithInvitees) or the callee's non-voice-only accept (AcceptIncomingImpl /
    // AcceptIncomingWithCharge); act on it here, exactly once, only after media_connected is the
    // same "connected" signal the UI above uses to show elapsed/"Connected" — never before the
    // callee has answered.
    // Stays pending (not dropped) while the camera button would be hidden — no encoder or the path
    // disallows video — so the camera never turns on without a way to turn it off; and on the
    // caller while the answer is only implicit (B30), so a voice answer still in flight is not
    // sent our video first.
    if (auto_camera_pending_ && auto_camera_call_id_ == active_call_id_ && media_connected) {
      const bool camera_offerable =
          backend->Media().VideoEncoderAvailable() && backend->Media().CameraPathAllowsVideo();
      auto awaiting_answer = backend->AwaitingExplicitAnswerForCall(active_call_id_);
      const bool answer_known = awaiting_answer && !*awaiting_answer;
      if (auto allowed = backend->VideoAllowedForCall(active_call_id_);
          allowed && allowed->has_value() && **allowed && camera_offerable && answer_known) {
        auto_camera_pending_ = false;
        if (!backend->Media().IsCameraEnabled()) {
          log().info << "auto camera on call_id=" << active_call_id_;
          backend->SetLocalVideoEnabled(true, WhileAlive([this, call_id = active_call_id_](Roe<void> cam) {
            if (!cam) {
              log().warning << "auto camera on failed call_id=" << call_id << ": " << cam.error().message;
            }
          }));
        }
      } else if (allowed && allowed->has_value() && !**allowed) {
        // Narrowed to voice-only before we got here — drop the intent; the camera-off sync in
        // ApplyAudioLevels below already owns turning the camera off for this case.
        auto_camera_pending_ = false;
      }
    }
    ApplyAudioLevels(backend->Media());
    {
      static std::string last_sub_log;
      const std::string sub = in_call.subtitle.c_str();
      if (sub != last_sub_log) {
        last_sub_log = sub;
        log().info
            << "in-call subtitle=\"" << sub << "\" phase="
            << CallPhaseName(backend->Phase())
            << " status=" << CallMediaStatusName(backend->MediaStatus())
            << " seat_media="
            << (seat_media == CallMediaSeat::MediaState::Live
                    ? "Live"
                    : seat_media == CallMediaSeat::MediaState::Connecting
                          ? "Connecting"
                          : seat_media == CallMediaSeat::MediaState::Failed ? "Failed" : "Idle")
            << " media_connected=" << (backend->Media().IsConnected() ? 1 : 0)
            << " media_active=" << (backend->Media().IsActive() ? 1 : 0)
            << " media_state=" << backend->Media().ConnectionState();
      }
    }
    SyncShellState();
    return;
  }

  ClearInCall();
  ClearRing();
  if (backend->Phase() == CallPhase::Ringing) {
    backend->Apply(CallLifecycleEvent::InviteCleared, {});
  }
  SyncShellState();
}

bool CallController::StartCall(const std::string& thread_id, const bool video_allowed) {
  return StartCallDirect(thread_id, video_allowed);
}

bool CallController::StartCallDirect(const std::string& thread_id, const bool video_allowed) {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    UserFeedback::Fail(Tr("call.error.unavailable"));
    return false;
  }
  auto thread = call_ports_.get_thread ? call_ports_.get_thread(thread_id)
                                       : Roe<std::optional<Thread>>::error(Error("call port unavailable"));
  if (!thread || !*thread) {
    UserFeedback::Fail(Tr("call.error.thread_not_found"));
    return false;
  }
  if ((*thread)->kind == ThreadKind::Group) {
    OpenGroupCallPicker(thread_id, video_allowed);
    return true;
  }
  if ((*thread)->kind != ThreadKind::Direct) {
    UserFeedback::Fail(Tr("call.error.wrong_thread_type"));
    return false;
  }
  if ((*thread)->peer_identity_value.empty()) {
    UserFeedback::Fail(Tr("call.error.missing_peer"));
    return false;
  }
  return StartCallWithInvitees(thread_id, video_allowed, {(*thread)->peer_identity_value});
}

bool CallController::StartCallWithInvitees(const std::string& thread_id, const bool video_allowed,
                                           const std::vector<std::string>& invitee_identities) {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    UserFeedback::Fail(Tr("call.error.unavailable"));
    return false;
  }
  if (invitee_identities.empty()) {
    UserFeedback::Fail(Tr("call.error.select_person"));
    return false;
  }
  // Requested: the calls owner starts it and reports back on UI (failure shown there).
  backend->StartCall(thread_id, video_allowed, invitee_identities,
                      WhileAlive([this, video_allowed](Roe<CallSession> started) {
    if (!started) {
      UserFeedback::Fail(PaymentErrorUserMessage(started.error().message));
      return;
    }
    active_call_id_ = started->call_id;
    const int64_t now = util::NowUnixMs();
    EmitMetrics(metrics_.NoteOutbound(started->call_id, video_allowed, now, Vitals(now)));
    if (video_allowed) {
      // 2026-09-28: caller picked "Video call" — turn the camera on automatically once the call
      // connects (supersedes V009's old "join video calls with camera off" default).
      auto_camera_pending_ = true;
      auto_camera_call_id_ = started->call_id;
    }
    RefreshPendingRing();
  }));
  return true;
}

void CallController::OpenGroupCallPicker(const std::string& thread_id, const bool video_allowed) {
  if (people_picker_notify_.open_for_group_call) {
    people_picker_notify_.open_for_group_call(thread_id, video_allowed);
  }
}

void CallController::OpenMidCallInvitePicker() {
  BindToMessaging();
  auto* backend = Backend();
  if (backend && backend->Available() && active_call_id_.empty()) {
    if (auto active = backend->ActiveLocalCall(); active && active->has_value()) {
      active_call_id_ = (*active)->call_id;
    }
  }
  if (active_call_id_.empty()) {
    UserFeedback::Fail(Tr("call.error.no_active"));
    return;
  }
  if (people_picker_notify_.open_for_call_add_guest) {
    people_picker_notify_.open_for_call_add_guest(active_call_id_);
  }
}

void CallController::InviteIdentitiesToActiveCall(const std::vector<std::string>& invitee_identities) {
  BindToMessaging();
  auto* backend = Backend();
  if (backend && backend->Available() && active_call_id_.empty()) {
    if (auto active = backend->ActiveLocalCall(); active && active->has_value()) {
      active_call_id_ = (*active)->call_id;
    }
  }
  if (!backend || !backend->Available() || active_call_id_.empty()) {
    UserFeedback::Fail(Tr("call.error.no_active"));
    return;
  }
  for (const std::string& identity : invitee_identities) {
    if (identity.empty()) {
      continue;
    }
    backend->InviteParticipant(active_call_id_, identity, WhileAlive([this](Roe<void> ok) {
      if (!ok) {
        UserFeedback::Fail(ok.error().message);
        return;
      }
      RefreshPendingRing();
    }));
  }
}

bool CallController::VideoCallAvailable() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return false;
  }
  return backend->Media().VideoEncoderAvailable();
}

void CallController::AcceptIncoming() {
  AcceptIncomingImpl(false);
}

void CallController::AcceptIncomingVoiceOnly() {
  AcceptIncomingImpl(true);
}

void CallController::AcceptIncomingImpl(const bool voice_only) {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }
  std::string call_id = ringing_call_id_;
  if (call_id.empty()) {
    call_id = ring_.call_id.c_str();
  }
  if (call_id.empty()) {
    call_id = backend->LastRingCallId();
  }
  if (call_id.empty()) {
    log().warning << "AcceptIncoming ignored (no call_id)";
    return;
  }
  backend->SetPendingAcceptChargeDecision(InitiationChargeDecision::Waive);
  backend->SetPendingAcceptVoiceOnly(voice_only);
  metrics_.NoteAccept(call_id, voice_only, util::NowUnixMs());
  if (!voice_only && ring_.video_allowed) {
    // 2026-09-28: callee answered a video call with "Video" (not narrowed to voice) — turn the
    // camera on automatically once the call connects, same as the caller.
    auto_camera_pending_ = true;
    auto_camera_call_id_ = call_id;
  } else if (auto_camera_call_id_ == call_id) {
    // Answered voice-only — never auto-enable the camera for this call.
    auto_camera_pending_ = false;
  }
  // Dismiss ring on the click frame (CALLS.md Accept → Accepting dismisses chrome). Leaving the
  // dialog up until AcceptInvite finishes made Accept look hung.
  ringtone_.Stop();
  ClearRing();
  SyncShellState();
  log().warning
      << "AcceptIncoming → lifecycle AcceptClicked call_id=" << call_id << " charge=waive"
      << (voice_only ? " voice_only=1" : "");
  backend->Apply(CallLifecycleEvent::AcceptClicked, call_id);
}

void CallController::AcceptIncomingWithCharge() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }
  if (!PaymentRailsAvailable()) {
    UserFeedback::Fail(Tr("call.ring.accept_charge_disabled_hint"));
    return;
  }
  std::string call_id = ringing_call_id_;
  if (call_id.empty()) {
    call_id = ring_.call_id.c_str();
  }
  if (call_id.empty()) {
    call_id = backend->LastRingCallId();
  }
  if (call_id.empty()) {
    log().warning << "AcceptIncomingWithCharge ignored (no call_id)";
    return;
  }
  backend->SetPendingAcceptChargeDecision(InitiationChargeDecision::TakeAll);
  metrics_.NoteAccept(call_id, false, util::NowUnixMs());
  backend->SetPendingAcceptVoiceOnly(false);
  if (ring_.video_allowed) {
    // 2026-09-28: charge-accept of a video call is still a "Video" accept — auto camera-on once
    // connected, same as AcceptIncomingImpl's non-voice-only path.
    auto_camera_pending_ = true;
    auto_camera_call_id_ = call_id;
  }
  ringtone_.Stop();
  ClearRing();
  SyncShellState();
  log().warning
      << "AcceptIncomingWithCharge → lifecycle AcceptClicked call_id=" << call_id << " charge=take_all";
  backend->Apply(CallLifecycleEvent::AcceptClicked, call_id);
}

void CallController::DeclineIncoming() {
  BindToMessaging();
  auto* backend = Backend();
  std::string call_id = ringing_call_id_;
  if (call_id.empty() && backend && backend->Available()) {
    call_id = backend->LastRingCallId();
  }
  if (call_id.empty()) {
    return;
  }
  ringtone_.Stop();
  ringing_call_id_.clear();
  ClearRing();
  SyncShellState();
  metrics_.NoteDecline(call_id);
  if (backend && backend->Available()) {
    backend->Apply(CallLifecycleEvent::DeclineClicked, call_id);
  }
}

void CallController::LeaveActive() {
  BindToMessaging();
  auto* backend = Backend();
  std::string call_id = active_call_id_;
  if (call_id.empty() && backend && backend->Available()) {
    call_id = backend->ActiveCallId();
  }
  if (call_id.empty()) {
    // Stale End button after Idle — force-clear chrome so Samsung does not look hung.
    ClearInCall();
    ClearRing();
    StopRingback(backend);
    SyncShellState();
    return;
  }
  // Detach SFU then stop SDL on UI before LeaveCall worker (must not Stop off-UI) and before
  // remounting away the Leave button. Media().Stop alone deadlocks if capture is in BlockingWrite.
  if (backend && backend->Available()) {
    backend->StopCallMedia(call_id);
  }
  active_call_id_.clear();
  ClearInCall();
  ClearRing();
  StopRingback(backend);
  SyncShellState();
  if (backend && backend->Available()) {
    backend->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  }
}

void CallController::RetryConnect() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }
  std::string call_id = active_call_id_;
  if (call_id.empty()) {
    call_id = backend->ActiveCallId();
  }
  if (call_id.empty()) {
    return;
  }
  backend->Apply(CallLifecycleEvent::RetryClicked, call_id);
  if (!backend->LastError().empty()) {
    UserFeedback::Fail(backend->LastError());
    backend->ClearLastError();
  }
  RefreshPendingRing();
}

void CallController::ToggleMute() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available() || !backend->Media().IsActive()) {
    return;
  }
  const bool before = backend->Media().IsMuted();
  backend->SetLocalAudioMuted(!before, WhileAlive([this](Roe<void> muted) {
    if (!muted) {
      UserFeedback::Fail(muted.error().message);
    }
    RefreshPendingRing();
  }));
}

void CallController::ToggleCamera() {
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available() || !backend->Media().IsActive()) {
    return;
  }
  if (backend->Available() && active_call_id_.empty()) {
    if (auto active = backend->ActiveLocalCall(); active && active->has_value()) {
      active_call_id_ = (*active)->call_id;
    }
  }
  const bool next = !backend->Media().IsCameraEnabled();
  if (next && !active_call_id_.empty()) {
    if (auto allowed = backend->VideoAllowedForCall(active_call_id_);
        !allowed || !allowed->has_value() || !**allowed) {
      UserFeedback::Fail(Tr("call.error.video_not_allowed"));
      return;
    }
  }
  backend->SetLocalVideoEnabled(next, WhileAlive([this](Roe<void> cam) {
    if (!cam) {
      UserFeedback::Fail(cam.error().message);
    }
    RefreshPendingRing();
  }));
}

void CallController::ToggleSpeaker() {
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
  RefreshPendingRing();
}

void CallController::ApplyAudioLevels(CallMediaEngine& media) {
  media.RefreshRemoteVideoHealth();

  auto& in_call = in_call_;
  const bool muted = media.IsMuted();
  in_call.muted = muted;
  if (auto failure = media.TakeCameraFailure()) {
    // Camera opens asynchronously (media device thread): report the miss and withdraw the video
    // flag the roster already advertised.
    UserFeedback::Fail(*failure);
    if (auto* failed_backend = Backend(); failed_backend && failed_backend->Available()) {
      failed_backend->SetLocalVideoEnabled(false);
    }
  }
  in_call.camera_on = media.IsCameraEnabled();
  if (in_call.camera_on && CameraRotationFollowsDevice()) {
    // iOS: video follows the phone's physical orientation (read on UI, per tick).
    const int rotation = CameraDisplayRotationDegrees();
    if (rotation != pushed_camera_rotation_) {
      media.UpdateCameraDisplayRotation(rotation);
      pushed_camera_rotation_ = rotation;
    }
  } else if (!in_call.camera_on && pushed_camera_rotation_ != -1) {
    StopCameraOrientationTracking();  // camera off: don't keep the motion sensors running
    pushed_camera_rotation_ = -1;
  }
  in_call.show_speaker = CallAudioSession::SupportsSpeakerToggle();
  in_call.speaker_on = CallAudioSession::IsSpeakerphoneOn();
  auto* backend = Backend();
  in_call.video_allowed = false;
  in_call.show_camera = false;
  if (backend && backend->Available() && !active_call_id_.empty()) {
    if (auto allowed = backend->VideoAllowedForCall(active_call_id_);
        allowed && allowed->has_value() && **allowed) {
      in_call.video_allowed = true;
      in_call.show_camera = media.VideoEncoderAvailable() && media.CameraPathAllowsVideo();
      camera_sync_off_done_ = false;
    } else if (!camera_sync_off_done_ && media.IsCameraEnabled() && allowed && allowed->has_value() &&
               !**allowed) {
      // M4: only narrow (turn camera off) when VideoAllowedForCall explicitly returned false — a
      // read error or unknown value keeps show_camera hidden above but must not touch the camera.
      // V035: a voice-only Accept (ours or the peer's) narrowed video_allowed to false after the
      // local camera was already on (e.g. caller turned it on before the callee answered voice-only)
      // — turn it off through the normal path, once.
      camera_sync_off_done_ = true;
      log().info << "ApplyAudioLevels camera off (video_allowed=false) call_id=" << active_call_id_;
      backend->SetLocalVideoEnabled(false, WhileAlive([](Roe<void> cam) {
        if (!cam) {
          UserFeedback::Fail(cam.error().message);
        }
      }));
    }
  }

  bool peer_camera_on = false;
  bool have_peer_video_flag = false;
  if (backend && backend->Available() && !active_call_id_.empty()) {
    if (auto peer_video = backend->PeerVideoEnabledForCall(active_call_id_);
        peer_video && peer_video->has_value()) {
      peer_camera_on = **peer_video;
      have_peer_video_flag = true;
    }
  }
  // Frames beat the roster (B58, device test 2026-09-29): the phone decoded the Mac's video at
  // 19 fps for a whole call and showed none of it, its roster row for the Mac still saying
  // "camera off" (the camera-on roster lost or overtaken by a stale one). A camera that really
  // turns off stops sending, so the "camera off" state still applies within kPeerVideoLiveMs.
  const bool peer_frames_live = media.IsRemoteVideoLive(kPeerVideoLiveMs);
  const char* roster_state = !have_peer_video_flag ? "none" : peer_camera_on ? "on" : "off";
  const char* frames_state = peer_frames_live ? "live" : "idle";
  if (std::string key = std::string(roster_state) + "/" + frames_state; key != peer_video_log_key_) {
    log().info << "peer video call_id=" << active_call_id_ << " roster=" << roster_state
               << " frames=" << frames_state;
    peer_video_log_key_ = std::move(key);
  }
  if (have_peer_video_flag && !peer_camera_on && peer_frames_live) {
    peer_camera_on = true;
  }
  // Kenneth 2026-09-29: after Accept the phone showed the audio layout for 2–4 s while both cameras
  // opened, so the tap looked like it had not registered. A video call shows the video stage from
  // Accept (callee) or connect (caller — not while still ringing out), with "Starting video…" until
  // the peer's first frame (not "Camera off": its roster says off until its camera opens), for at
  // most kVideoStartMs after connect.
  bool starting_video = false;
  if (backend && backend->Available() && !active_call_id_.empty() && auto_camera_call_id_ == active_call_id_ &&
      !media.EverHadRemoteVideo() && backend->Phase() != CallPhase::OutboundCalling) {
    if (auto allowed = backend->VideoAllowedForCall(active_call_id_); allowed && allowed->has_value() && **allowed) {
      const int64_t now = util::NowUnixMs();
      const bool live = backend->MediaChromeLive();
      if (live && video_start_since_ms_ == 0) {
        video_start_since_ms_ = now;
      }
      starting_video = !live || now - video_start_since_ms_ < kVideoStartMs;
    }
  }
  if (have_peer_video_flag && !peer_camera_on) {
    media.ClearRemoteVideo();
  }

  const bool stalling = media.IsRemoteVideoStalling();
  const std::string conn = media.ConnectionState();
  const bool expect_remote_video = !have_peer_video_flag || peer_camera_on;
  const bool missing_after_video =
      media.EverHadRemoteVideo() && expect_remote_video && !media.HasRemoteVideo();
  const bool mesh_messaging_failed =
      backend && backend->Available() && backend->IsP2pConnectFailed() &&
      !backend->IsAwaitingSfuRecovery() && !media.IsSfuMode();
  const bool media_reconnect =
      !mesh_messaging_failed && (stalling || missing_after_video || conn == "disconnected");

  // Live or soft-stall: keep painting the last frame. Hard stall / camera-off clears HasRemoteVideo.
  in_call.remote_video = media.HasRemoteVideo() && expect_remote_video;
  if (!in_call.remote_video) {
    CallVideoTileRenderer::Instance().ClearRemote();
  }

  CallMediaEngine::VideoTileFrame local_tile;
  in_call.local_preview = media.CopyLocalVideoFrame(local_tile);
  if (in_call.local_preview) {
    CallVideoTileRenderer::Frame frame;
    frame.width = local_tile.width;
    frame.height = local_tile.height;
    frame.seq = local_tile.seq;
    frame.rgba = std::move(local_tile.rgba);
    CallVideoTileRenderer::Instance().SubmitLocalFrame(std::move(frame));
  }
  if (in_call.remote_video) {
    CallMediaEngine::VideoTileFrame remote_tile;
    if (media.CopyRemoteVideoFrame(remote_tile)) {
      CallVideoTileRenderer::Frame frame;
      frame.width = remote_tile.width;
      frame.height = remote_tile.height;
      frame.seq = remote_tile.seq;
      frame.rgba = std::move(remote_tile.rgba);
      CallVideoTileRenderer::Instance().SubmitRemoteFrame(std::move(frame));
    }
  }

  std::vector<uint32_t> live_peer_streams;
  const int64_t now_ms = util::NowUnixMs();
  for (CallRosterParticipantState& row : in_call.roster) {
    if (row.is_local) {
      row.has_remote_video = false;
      continue;
    }
    const uint32_t stream = static_cast<uint32_t>(std::strtoul(row.stream_id.c_str(), nullptr, 10));
    row.has_remote_video = stream != 0 && media.HasRemoteVideoForStream(stream);
    if (!row.has_remote_video) {
      continue;
    }
    live_peer_streams.push_back(stream);
    CallMediaEngine::VideoTileFrame peer_tile;
    if (!media.CopyRemoteVideoFrameForStream(stream, peer_tile)) {
      continue;
    }
    CallVideoTileRenderer::Frame frame;
    frame.width = peer_tile.width;
    frame.height = peer_tile.height;
    frame.seq = peer_tile.seq;
    frame.rgba = std::move(peer_tile.rgba);
    CallVideoTileRenderer::Instance().SubmitPeerFrame(stream, std::move(frame));
  }
  CallVideoTileRenderer::Instance().RetainPeers(live_peer_streams);

  if (backend && backend->Available() && !active_call_id_.empty()) {
    for (uint32_t stream : media.TakePendingVideoRefreshStreamIds()) {
      for (const CallRosterParticipantState& row : in_call.roster) {
        if (row.is_local) {
          continue;
        }
        const uint32_t row_stream =
            static_cast<uint32_t>(std::strtoul(row.stream_id.c_str(), nullptr, 10));
        if (row_stream != stream) {
          continue;
        }
        backend->RequestVideoRefresh(active_call_id_, row.identity.c_str());
        break;
      }
    }
    const bool hard_stall = missing_after_video && !media.HasRemoteVideo();
    if ((hard_stall || stalling) && now_ms - last_video_refresh_ms_ >= 2000) {
      last_video_refresh_ms_ = now_ms;
      if (in_call.show_roster) {
        for (const CallRosterParticipantState& row : in_call.roster) {
          if (row.is_local || !row.video_enabled || row.has_remote_video) {
            continue;
          }
          backend->RequestVideoRefresh(active_call_id_, row.identity.c_str());
        }
      } else if (auto peer = backend->PeerIdentityForCall(active_call_id_); peer && peer->has_value()) {
        backend->RequestVideoRefresh(active_call_id_, **peer);
      }
    }
  }

  in_call.stage_visible = in_call.camera_on || in_call.remote_video || in_call.local_preview ||
                          (have_peer_video_flag && peer_camera_on) || media_reconnect || starting_video;
  if (in_call.remote_video) {
    in_call.remote_placeholder = "";
  } else if (starting_video) {
    in_call.remote_placeholder = Tr("call.placeholder.starting_video").c_str();
  } else if (have_peer_video_flag && !peer_camera_on) {
    in_call.remote_placeholder = Tr("call.placeholder.camera_off").c_str();
  } else if (mesh_messaging_failed) {
    in_call.remote_placeholder = Tr("call.status.couldnt_connect").c_str();
  } else if (media_reconnect) {
    in_call.remote_placeholder = Tr("call.status.reconnecting").c_str();
  } else {
    in_call.remote_placeholder = "";
  }

  in_call.mic_level = muted ? 0 : QuantizeAudioLevel(media.LocalInputLevel());
  in_call.peer_level = QuantizeAudioLevel(media.RemoteOutputLevel());
  if (!media.HasLocalCapture() && !muted) {
    in_call.mic_hint = Tr("call.level.silent").c_str();
  } else {
    in_call.mic_hint = LevelHint(in_call.mic_level, false, muted).c_str();
  }
  in_call.peer_hint = LevelHint(in_call.peer_level, true, false).c_str();
  if (mesh_messaging_failed) {
    in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
  } else if (media_reconnect && !media.IsConnected()) {
    in_call.subtitle = Tr("call.status.reconnecting").c_str();
  } else if (stalling) {
    in_call.subtitle = Tr("call.status.reconnecting").c_str();
  } else if (backend && backend->Available() &&
             (backend->MediaChromeLive() || backend->SeatMediaLive(active_call_id_))) {
    // Prefer V037 MediaChromeLive; seat Live remains fallback during Status lag.
    // Provisional Connected — ApplyMediaHealth may override for NoAudio / TX-only / Degraded.
    in_call.elapsed = FormatElapsed(media.ConnectedAtMs());
    if (!in_call.elapsed.empty()) {
      in_call.subtitle = in_call.elapsed;
    } else {
      in_call.subtitle = Tr("call.status.connected").c_str();
    }
  }

  ApplyMediaHealth(media, backend, media_reconnect || mesh_messaging_failed);
}

CallMediaHealthView CallController::BuildMediaHealthView(CallMediaEngine& media, CallUiBackend* backend,
                                                         const bool media_reconnect) const {
  CallMediaHealthInput in;
  in.engine = media.HealthSnapshot();
  if (backend && backend->Available()) {
    in.hop = backend->HopHealth();
    in.reach_path_kind = backend->MediaPathKind();
  }
  in.now_ms = util::NowUnixMs();
  in.reconnecting = media_reconnect;
  return EvaluateCallMediaHealth(in);
}

void CallController::ApplyMediaHealth(CallMediaEngine& media, CallUiBackend* backend,
                                      const bool media_reconnect) {
  auto& in_call = in_call_;
  if (!in_call.active) {
    return;
  }

  const CallMediaHealthView view = BuildMediaHealthView(media, backend, media_reconnect);
  const int64_t now_ms = util::NowUnixMs();

  if (metrics_.Tracking() && now_ms - last_metrics_media_ms_ >= 500) {
    last_metrics_media_ms_ = now_ms;
    CallMetricsTracker::MediaTick tick;
    tick.connected = backend && backend->Available() && backend->MediaChromeLive();
    tick.path = view.path_kind;
    tick.quality = view.quality;
    tick.health = media.HealthSnapshot();
    EmitMetrics(metrics_.NoteMedia(active_call_id_, tick, now_ms, Vitals(now_ms)));
  }

  in_call.quality_bars = view.quality_bars;
  in_call.quality_ok = view.quality <= CallPathQuality::Good;
  in_call.quality_warn = view.quality == CallPathQuality::Fair;
  in_call.quality_error =
      view.quality == CallPathQuality::Poor || view.quality == CallPathQuality::NoAudio ||
      view.quality == CallPathQuality::Reconnecting;

  if (const char* label_key = CallPathQualityLabelKey(view.quality); label_key && label_key[0]) {
    in_call.quality_label = Tr(label_key).c_str();
  } else {
    in_call.quality_label = "";
  }
  if (const char* hint_key = CallAudioAsymmetryHintKey(view.asymmetry); hint_key && hint_key[0]) {
    in_call.quality_hint = Tr(hint_key).c_str();
  } else {
    in_call.quality_hint = "";
  }

  // Seat Live / Status Live can still be TX-only (NAT one-way) — don't keep Connected claim.
  const bool media_broken =
      view.quality == CallPathQuality::NoAudio ||
      view.asymmetry == CallAudioAsymmetry::SendingOnly ||
      view.asymmetry == CallAudioAsymmetry::ReceivingOnly ||
      (backend && backend->Available() &&
       backend->MediaStatus() == CallMediaStatus::DegradedTxOnly);
  // Reconnecting says it all: no path means no audio either way.
  if (media_broken && !(backend && backend->Available() && backend->MediaStatus() == CallMediaStatus::Reconnecting)) {
    if (const char* hint_key = CallAudioAsymmetryHintKey(view.asymmetry); hint_key && hint_key[0]) {
      in_call.subtitle = Tr(hint_key).c_str();
    } else if (const char* label_key = CallPathQualityLabelKey(view.quality); label_key &&
                                                                              label_key[0]) {
      in_call.subtitle = Tr(label_key).c_str();
    }
  }

  // Always-on short path word next to elapsed / connected / health subtitle.
  const char* path_key = nullptr;
  if (view.path_kind == "media_relay" || view.path_kind == "relay") {
    path_key = "call.details.path.media_relay";
  } else if (view.path_kind == "circuit") {
    path_key = "call.details.path.circuit";
  } else if (view.path_kind == "punched") {
    path_key = "call.details.path.punched";
  } else if (view.path_kind == "direct") {
    path_key = "call.details.path.direct";
  }
  const bool show_path =
      path_key && backend && backend->Available() &&
      (backend->MediaChromeLive() || backend->SeatMediaLive(active_call_id_) ||
       backend->MediaStatus() == CallMediaStatus::DegradedTxOnly);
  if (show_path) {
    const std::string path_label = Tr(path_key);
    if (!in_call.subtitle.empty() && in_call.subtitle != path_label) {
      in_call.subtitle = (std::string(in_call.subtitle.c_str()) + " · " + path_label).c_str();
    } else if (in_call.subtitle.empty()) {
      in_call.subtitle = path_label.c_str();
    }
  }

  const bool diagnostics =
      call_ports_.call_diagnostics_enabled && call_ports_.call_diagnostics_enabled();
  in_call.show_debug_subtitle = diagnostics && media.IsActive();
  if (in_call.show_debug_subtitle) {
    in_call.debug_subtitle = FormatCallDebugSubtitle(view, now_ms).c_str();
  } else {
    in_call.debug_subtitle = "";
  }

  if (now_ms - last_media_health_log_ms_ >= 2000) {
    last_media_health_log_ms_ = now_ms;
    log().info << FormatMediaHealthLogLine(view, now_ms, active_call_id_);
  }

  // Toast only real, lasting audio faults (Kenneth 2026-09-28: the old toasts fired on every dip —
  // "Poor" on a jitter spike, "can't hear the other side" in the second before the first audio
  // arrived). Network quality stays visible in the signal bars; an audio fault must persist for
  // CallAudioFaultToastGate::kAudioFaultToastMs and toasts at most once per call.
  const bool audio_fault =
      view.quality == CallPathQuality::NoAudio || view.asymmetry != CallAudioAsymmetry::None;
  if (audio_fault_gate_.Update(active_call_id_, audio_fault, now_ms)) {
    if (const char* hint_key = CallAudioAsymmetryHintKey(view.asymmetry); hint_key && hint_key[0]) {
      UserFeedback::Fail(Tr(hint_key));
    } else if (const char* label_key = CallPathQualityLabelKey(view.quality); label_key && label_key[0]) {
      UserFeedback::Fail(Tr(label_key));
    }
  }
}

void CallController::ShowCallDetails() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { ShowCallDetails(); });
    return;
  }
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }
  auto& in_call = in_call_;
  if (!in_call.active) {
    return;
  }

  auto& media = backend->Media();
  const bool media_reconnect = !media.IsConnected() && media.IsActive();
  const CallMediaHealthView view = BuildMediaHealthView(media, backend, media_reconnect);
  const int64_t now_ms = util::NowUnixMs();
  const bool diagnostics =
      call_ports_.call_diagnostics_enabled && call_ports_.call_diagnostics_enabled();

  CallDetailsCopy copy;
  copy.elapsed = in_call.elapsed.empty() ? std::string(in_call.subtitle.c_str())
                                         : std::string(in_call.elapsed.c_str());
  copy.path_label = view.path_kind == "media_relay" || view.path_kind == "relay"
                        ? Tr("call.details.path.media_relay")
                        : (view.path_kind == "circuit"
                               ? Tr("call.details.path.circuit")
                               : (view.path_kind == "punched" ? Tr("call.details.path.punched")
                                                             : Tr("call.details.path.direct")));
  copy.quality_label = Tr(CallPathQualityDetailsLabelKey(view.quality));
  copy.mic_label = LevelHint(in_call.mic_level, false, in_call.muted);
  copy.incoming_label = LevelHint(in_call.peer_level, true, false);
  if (const char* hint_key = CallAudioAsymmetryHintKey(view.asymmetry); hint_key && hint_key[0]) {
    copy.asymmetry_hint = Tr(hint_key);
  }
  copy.call_id = active_call_id_;
  copy.duration_heading = Tr("call.details.duration");
  copy.path_heading = Tr("call.details.path");
  copy.quality_heading = Tr("call.details.quality");
  copy.mic_heading = Tr("call.details.mic");
  copy.incoming_heading = Tr("call.details.incoming");
  copy.note_heading = Tr("call.details.note");
  copy.diagnostics_heading = Tr("call.details.diagnostics");

  const std::string body = FormatCallDetailsText(view, now_ms, diagnostics, copy);
  UserFeedback::Confirm(
      Tr("call.details.title"), body,
      [body](const bool ok) {
        if (!ok) {
          return;
        }
        if (ui::SystemInterface* system = ui::GetSystemInterface()) {
          system->SetClipboardText(body.c_str());
          UserFeedback::Ok(Tr("call.details.copied"));
        }
      },
      Tr("call.details.copy"));
}

void CallController::RefreshCallLevels() {
  if (active_call_id_.empty()) {
    return;
  }
  auto* backend = Backend();
  if (!backend || !backend->Available()) {
    return;
  }
  // Keep Calling… / timer / levels fresh even before media starts.
  RefreshPendingRing();
}

} // namespace pbr
