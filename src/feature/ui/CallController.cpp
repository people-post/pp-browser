#include <stdexcept>
#include "feature/ui/CallController.h"

#include "base/i18n/LocalizationService.h"
#include "base/media/CallAudioSession.h"
#include "base/media/CallMediaEngine.h"
#include "base/media/CallMediaHealth.h"
#include "base/messaging/CallTypes.h"
#include "base/people/ContactTypes.h"
#include "base/runtime/AppRuntime.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/Platform.h"
#include "base/platform/PlatformUserHints.h"
#include "base/runtime/ProductBranding.h"
#include "base/ui/ShellTypes.h"
#include "feature/messaging/CallLifecycle.h"
#include "feature/messaging/CallSessionManager.h"
#include "feature/messaging/MessagingCallPorts.h"
#include "feature/ui/CallChromeSync.h"
#include "feature/ui/CallConflictCopy.h"
#include "feature/ui/PeoplePickerNotifyPorts.h"
#include "CallVideoTileRenderer.h"
#include "feature/ui/UserFeedback.h"

#include "common/Utilities.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace pbr {

CallController::CallController() {
  redirectLogger("CallController");
}

namespace {

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
      .ring_call_id = ring.call_id.c_str(),
      .in_call_id = in_call.call_id.c_str(),
      .in_call_subtitle = in_call.subtitle.c_str(),
      .ring_caller_label = ring.caller_label.c_str(),
      .ring_media_label = ring.media_label.c_str(),
      .ring_eyebrow = ring.eyebrow.c_str(),
      .ring_conflict_hint = ring.conflict_hint.c_str(),
      .ring_accept_label = ring.accept_label.c_str(),
      .ring_decline_label = ring.decline_label.c_str(),
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

void CallController::BindCallPorts(MessagingCallPorts ports) {
  call_ports_ = std::move(ports);
  BindToMessaging();
}

void CallController::BindShellCallChrome(ShellCallChromePorts ports) {
  shell_call_chrome_ = std::move(ports);
}

void CallController::BindPeoplePickerNotify(PeoplePickerNotifyPorts ports) {
  people_picker_notify_ = std::move(ports);
}

bool CallController::MessagingInitialized() const {
  return call_ports_.initialized && call_ports_.initialized();
}

CallSessionManager* CallController::Calls() {
  return call_ports_.calls ? call_ports_.calls() : nullptr;
}

CallLifecycle* CallController::Lifecycle() {
  return call_ports_.lifecycle ? call_ports_.lifecycle() : nullptr;
}

void CallController::BindToMessaging() {
  if (!MessagingInitialized()) {
    bound_calls_ = nullptr;
    return;
  }
  auto* calls = Calls();
  if (!calls) {
    bound_calls_ = nullptr;
    return;
  }
  // CallSessionManager is recreated in BuildMessagingStack; rebind when the pointer changes.
  if (bound_calls_ == calls) {
    return;
  }
  calls->SetOnRingChanged([this]() {
    // Ingest may run on IO; shell/RmlUi updates must stay on UI.
    AppRuntime::PostUI([this]() { RefreshPendingRing(); });
  });
  if (auto* life = Lifecycle()) {
    life->SetOnChromeRefresh([this]() { RefreshPendingRing(); });
  }
  bound_calls_ = calls;
  // Pick up post-restart abandon / pending ring after stack rebuild.
  RefreshPendingRing();
}

void CallController::Tick() {
  BindToMessaging();
  if (auto* calls = Calls()) {
    calls->SweepExpiredInvites();
  }
  const int64_t now = util::NowUnixMs();
  if (!shell_call_chrome_.call_ring) {
    return;
  }
  auto& ring = shell_call_chrome_.call_ring();
  if (ring.active) {
    if (now - last_pulse_toggle_ms_ >= 600) {
      ring.pulse = !ring.pulse;
      last_pulse_toggle_ms_ = now;
      SyncShellState();
    }
  }
  RefreshCallLevels();
  SyncRingtone();
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
  if (shell_call_chrome_.call_ring) {
    shell_call_chrome_.call_ring() = {};
  }
  ringtone_.Stop();
}

void CallController::ClearInCall() {
  active_call_id_.clear();
  chrome_mode_ = CallChromeMode::Expanded;
  restore_mode_ = CallChromeMode::Expanded;
  minimized_corner_ = 0;
  chrome_mode_call_id_.clear();
  last_media_health_log_ms_ = 0;
  last_warned_quality_ = -1;
  if (shell_call_chrome_.call_in_progress) {
    shell_call_chrome_.call_in_progress() = {};
  }
  CallVideoTileRenderer::Instance().Clear();
}

void CallController::HideInCallChrome() {
  if (shell_call_chrome_.call_in_progress) {
    shell_call_chrome_.call_in_progress() = {};
  }
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
  if (shell_call_chrome_.call_in_progress && shell_call_chrome_.call_in_progress().active) {
    chrome_mode_call_id_ = shell_call_chrome_.call_in_progress().call_id.c_str();
    ApplyChromeModeToState(shell_call_chrome_.call_in_progress());
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
  if (shell_call_chrome_.call_in_progress && shell_call_chrome_.call_in_progress().active) {
    shell_call_chrome_.call_in_progress().minimized_corner = minimized_corner_;
    SyncShellState();
  }
}

void CallController::SyncShellState() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { SyncShellState(); });
    return;
  }
  if (!shell_call_chrome_.call_ring || !shell_call_chrome_.call_in_progress) {
    return;
  }
  const CallChromeLayer next =
      CaptureCallChrome(shell_call_chrome_.call_ring(), shell_call_chrome_.call_in_progress());
  const CallChromeUpdate update = ClassifyCallChromeUpdate(synced_chrome_, next);
  synced_chrome_ = next;

  if (update == CallChromeUpdate::None) {
    return;
  }
  // ShellHost owns remount / DirtyCallChrome / force-frame (not grab-bag DirtyWindow).
  if (shell_call_chrome_.apply_chrome_update) {
    shell_call_chrome_.apply_chrome_update(update);
  }
}

void CallController::SyncRingtone() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { SyncRingtone(); });
    return;
  }
  // Mobile: SDL playback teardown from a worker can stall the SDL thread during Accept.
  // Skip ringtone until that path is safe; signaling/listen dogfood does not need it.
  if (Platform::IsMobile()) {
    if (ringtone_.IsPlaying()) {
      ringtone_.Stop();
    }
    return;
  }
  const bool should_ring = shell_call_chrome_.call_ring && shell_call_chrome_.call_ring().active;
  if (should_ring && !ringtone_.IsPlaying()) {
    ringtone_.Start();
  } else if (!should_ring && ringtone_.IsPlaying()) {
    ringtone_.Stop();
  }
}

std::string CallController::DisplayNameForIdentity(const std::string& identity) const {
  if (identity.empty() || !call_ports_.find_contact_by_identity) {
    return {};
  }
  if (auto contact = call_ports_.find_contact_by_identity(identity, ContactIdKind::RelayUser)) {
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
  auto* calls = Calls();
  if (!calls) {
    return;
  }

  if (auto* life = Lifecycle(); life && !life->LastError().empty()) {
    UserFeedback::Fail(life->LastError());
    life->ClearLastError();
  }

  if (auto media_err = calls->TakeLastMediaError(); media_err && !media_err->empty()) {
    // SoftMigrate / hop failures need a sticky banner — toast (even Long=6s) vanishes before
    // users can read multi-hop diagnostics.
    UserFeedback::NeedsSetup(*media_err);
  }

  calls->PollPendingSfuAttach();
  calls->PollP2pConnectHealth();

  auto top = calls->TopPendingInvite();
  if (top && top->has_value()) {
    CallLifecycle* life = Lifecycle();
    const bool accept_in_flight = life && life->ShouldSuppressRing((*top)->call_id);
    auto active = calls->ActiveLocalCall();
    const bool same_call_active =
        active && active->has_value() && (*active)->call_id == (*top)->call_id;

    // Accept in flight (CALLS.md): dismiss ring immediately; show Connecting bar so Accept
    // does not look hung while AcceptInvite runs on the worker.
    if (accept_in_flight && !same_call_active) {
      ringing_call_id_ = (*top)->call_id;
      ClearRing();
      if (shell_call_chrome_.call_in_progress) {
        auto& in_call = shell_call_chrome_.call_in_progress();
        in_call.active = true;
        in_call.call_id = (*top)->call_id;
        in_call.subtitle = Tr("call.status.connecting").c_str();
        in_call.title = (*top)->media_mode == CallMediaMode::Video ? Tr("call.title.video").c_str()
                                                                   : Tr("call.title.voice").c_str();
        const std::string caller = DisplayNameForIdentity((*top)->inviter_identity);
        in_call.peer_label = caller.empty() ? (*top)->inviter_identity.c_str() : caller.c_str();
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
      last_ring_call_id_ = ringing_call_id_;
      if (life) {
        life->NoteRingCallId(ringing_call_id_);
        if (life->Phase() == CallPhase::Idle) {
          life->Apply(CallLifecycleEvent::InviteSeen, ringing_call_id_);
        }
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
        if (auto peer = calls->PeerIdentityForCall((*active)->call_id); peer && peer->has_value()) {
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

      auto& ring = shell_call_chrome_.call_ring();
      const bool was_active = ring.active;
      ring.active = true;
      ring.conflict = has_conflict;
      ring.call_id = (*top)->call_id;
      ring.caller_label = caller_label;
      ring.media_label = (*top)->media_mode == CallMediaMode::Video
                             ? Tr("call.ring.incoming_video").c_str()
                             : Tr("call.ring.incoming_voice").c_str();
      if (!was_active) {
        log().warning
            << "RefreshPendingRing activate call_id=" << ringing_call_id_;
      }
      ring.eyebrow = copy.eyebrow;
      ring.conflict_hint = copy.hint;
      ring.accept_label = copy.accept_label;
      ring.decline_label = copy.decline_label;
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

  if (auto active = calls->ActiveLocalCall(); active && active->has_value()) {
    CallLifecycle* life = Lifecycle();
    // LeaveClicked sets Idle before LeaveCall IO finishes — do not resurrect the panel from the
    // still-Active disk row (Samsung: End looked hung / "couldn't connect" stuck).
    if (life && life->Phase() == CallPhase::Idle) {
      if ((*active)->state == CallSessionState::Active && !calls->Media().IsActive() &&
          !calls->MediaAttemptedThisProcess((*active)->call_id) && !calls->IsAwaitingSfuRecovery()) {
        // True orphan after force-quit / process restart.
        (void)calls->LeaveCall((*active)->call_id);
      }
      active_call_id_.clear();
      ClearInCall();
      ClearRing();
      SyncShellState();
      return;
    }

    active_call_id_ = (*active)->call_id;
    ClearRing();

    // Unanswered outbound: offerer stays Joined until Leave — without a TTL the Calling bar
    // sticks forever and masks a reverse inbound ring as "previous or new call?".
    if (life && life->Phase() == CallPhase::OutboundCalling && !calls->Media().IsActive() &&
        (*active)->created_at > 0 &&
        util::NowUnixMs() - (*active)->created_at >= kDefaultCallInviteTtlMs) {
      log().warning
          << "outbound unanswered timeout call_id=" << (*active)->call_id;
      life->Apply(CallLifecycleEvent::LeaveClicked, (*active)->call_id);
      return;
    }

    // Direct connect failed: keep chrome for Retry/End on 1:1. Group SFU recovery keeps chrome too.
    // Do not auto-LeaveCall on `failed` — that erased the session before the user could retry.
    if (calls->Media().IsActive() && calls->Media().ActiveCallId() == active_call_id_) {
      const std::string media_state = calls->Media().ConnectionState();
      if (media_state == "failed" && !calls->IsAwaitingSfuRecovery() && !calls->Media().IsSfuMode() &&
          !calls->IsP2pConnectFailed()) {
        // State callback may not have marked yet (ordering); ensure UI can show Retry.
        calls->PollP2pConnectHealth();
      }
    }

    auto& in_call = shell_call_chrome_.call_in_progress();
    in_call.active = true;
    in_call.call_id = (*active)->call_id;
    in_call.muted = calls->Media().IsMuted();
    in_call.show_speaker = CallAudioSession::SupportsSpeakerToggle();
    in_call.speaker_on = CallAudioSession::IsSpeakerphoneOn();

    const bool is_video = (*active)->media_mode == CallMediaMode::Video;
    int joined_count = 0;
    std::string local_identity;
    if (call_ports_.local_relay_identity) {
      local_identity = call_ports_.local_relay_identity().value_or(std::string{});
    }
    if (auto participants = calls->ListJoinedParticipants((*active)->call_id); participants) {
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
    if (auto peer = calls->PeerIdentityForCall((*active)->call_id); peer && peer->has_value()) {
      const std::string name = DisplayNameForIdentity(**peer);
      if (!name.empty()) {
        peer_label = name;
      }
    }
    in_call.peer_label = in_call.show_roster ? Tr("call.label.others").c_str() : peer_label.c_str();

    const bool p2p_failed = calls->IsP2pConnectFailed();
    const bool group_call_context = (*active)->origin_group_id.has_value() || joined_count > 2 ||
                                    calls->IsAwaitingSfuRecovery() || calls->Media().IsSfuMode();
    in_call.show_retry = p2p_failed && !calls->IsAwaitingSfuRecovery() && !calls->Media().IsSfuMode();

    // Prefer media IsConnected; also trust lifecycle InCall once DirectConnected fired so
    // chrome cannot stick on Connecting while Opus already flows (connection_state lag).
    // Media activity (hop find/switch) wins over Connected so SoftMigrate progress stays visible
    // while the old path is still up.
    const bool media_connected = calls->Media().IsConnected() ||
                                 (life && life->Phase() == CallPhase::InCall && calls->Media().IsActive());
    const std::string activity = calls->PeekMediaActivity();
    if (p2p_failed) {
      in_call.elapsed = {};
      in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
      in_call.status_hint =
          group_call_context ? ComposeGroupCallStatusHint().c_str()
                             : ComposeP2pStatusHint(calls->P2pConnectMissingMic()).c_str();
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
    } else if (media_connected) {
      calls->ClearMediaActivity();
      in_call.elapsed = FormatElapsed(calls->Media().ConnectedAtMs());
      in_call.subtitle = in_call.elapsed.empty() ? Tr("call.status.connected").c_str() : in_call.elapsed;
      in_call.show_retry = false;
      in_call.status_hint = {};
    } else {
      in_call.elapsed = {};
      in_call.status_hint = {};
      if (!calls->Media().IsActive()) {
        in_call.subtitle = Tr("call.status.calling").c_str();
      } else if (calls->IsSoftMigrateInFlight()) {
        in_call.subtitle = Tr("call.status.setting_up_group").c_str();
      } else if (calls->IsSfuAttachWaitActive()) {
        in_call.subtitle = Tr("call.status.waiting_for_media_path").c_str();
      } else {
        const std::string state = calls->Media().ConnectionState();
        if (calls->IsAwaitingSfuRecovery()) {
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
    ApplyAudioLevels(calls->Media());
    {
      static std::string last_sub_log;
      const std::string sub = in_call.subtitle.c_str();
      if (sub != last_sub_log) {
        last_sub_log = sub;
        log().info
            << "in-call subtitle=\"" << sub << "\" phase="
            << (life ? CallPhaseName(life->Phase()) : "?")
            << " media_connected=" << (calls->Media().IsConnected() ? 1 : 0)
            << " media_active=" << (calls->Media().IsActive() ? 1 : 0)
            << " media_state=" << calls->Media().ConnectionState();
      }
    }
    SyncShellState();
    return;
  }

  ClearInCall();
  ClearRing();
  if (auto* life = Lifecycle(); life && life->Phase() == CallPhase::Ringing) {
    life->Apply(CallLifecycleEvent::InviteCleared, {});
  }
  SyncShellState();
}

bool CallController::StartCall(const std::string& thread_id, const bool video) {
  BindToMessaging();
  auto* calls = Calls();
  if (!calls) {
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
    OpenGroupCallPicker(thread_id, video);
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
  return StartCallWithInvitees(thread_id, video, {(*thread)->peer_identity_value});
}

bool CallController::StartCallWithInvitees(const std::string& thread_id, const bool video,
                                           const std::vector<std::string>& invitee_identities) {
  BindToMessaging();
  auto* calls = Calls();
  if (!calls) {
    UserFeedback::Fail(Tr("call.error.unavailable"));
    return false;
  }
  if (invitee_identities.empty()) {
    UserFeedback::Fail(Tr("call.error.select_person"));
    return false;
  }
  auto started =
      calls->StartCall(thread_id, video ? CallMediaMode::Video : CallMediaMode::Voice, invitee_identities);
  if (!started) {
    UserFeedback::Fail(started.error().message);
    return false;
  }
  active_call_id_ = started->call_id;
  if (auto* life = Lifecycle()) {
    life->Apply(CallLifecycleEvent::OutboundStarted, started->call_id);
  }
  RefreshPendingRing();
  return true;
}

void CallController::OpenGroupCallPicker(const std::string& thread_id, const bool video) {
  if (people_picker_notify_.open_for_group_call) {
    people_picker_notify_.open_for_group_call(thread_id, video);
  }
}

void CallController::OpenMidCallInvitePicker() {
  BindToMessaging();
  auto* calls = Calls();
  if (calls && active_call_id_.empty()) {
    if (auto active = calls->ActiveLocalCall(); active && active->has_value()) {
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
  auto* calls = Calls();
  if (calls && active_call_id_.empty()) {
    if (auto active = calls->ActiveLocalCall(); active && active->has_value()) {
      active_call_id_ = (*active)->call_id;
    }
  }
  if (!calls || active_call_id_.empty()) {
    UserFeedback::Fail(Tr("call.error.no_active"));
    return;
  }
  int invited = 0;
  for (const std::string& identity : invitee_identities) {
    if (identity.empty()) {
      continue;
    }
    if (auto ok = calls->InviteParticipant(active_call_id_, identity); ok) {
      ++invited;
    } else {
      UserFeedback::Fail(ok.error().message);
      break;
    }
  }
  if (invited > 0) {
    RefreshPendingRing();
  }
}

bool CallController::StartVoiceCall(const std::string& thread_id) {
  return StartCall(thread_id, false);
}

bool CallController::StartVideoCall(const std::string& thread_id) {
  return StartCall(thread_id, true);
}

void CallController::AcceptIncoming() {
  BindToMessaging();
  auto* life = Lifecycle();
  if (!life) {
    return;
  }
  std::string call_id = ringing_call_id_;
  if (call_id.empty() && shell_call_chrome_.call_ring) {
    call_id = shell_call_chrome_.call_ring().call_id;
  }
  if (call_id.empty()) {
    call_id = last_ring_call_id_;
  }
  if (call_id.empty()) {
    call_id = life->LastRingCallId();
  }
  if (call_id.empty()) {
    log().warning << "AcceptIncoming ignored (no call_id)";
    return;
  }
  // Dismiss ring on the click frame (CALLS.md Accept → Accepting dismisses chrome). Leaving the
  // dialog up until AcceptInvite finishes made Accept look hung.
  ringtone_.Stop();
  ClearRing();
  SyncShellState();
  log().warning
      << "AcceptIncoming → lifecycle AcceptClicked call_id=" << call_id;
  life->Apply(CallLifecycleEvent::AcceptClicked, call_id);
}

void CallController::DeclineIncoming() {
  BindToMessaging();
  auto* life = Lifecycle();
  std::string call_id = ringing_call_id_;
  if (call_id.empty() && life) {
    call_id = life->LastRingCallId();
  }
  if (call_id.empty()) {
    return;
  }
  ringtone_.Stop();
  ringing_call_id_.clear();
  ClearRing();
  SyncShellState();
  if (life) {
    life->Apply(CallLifecycleEvent::DeclineClicked, call_id);
  }
}

void CallController::LeaveActive() {
  BindToMessaging();
  auto* life = Lifecycle();
  std::string call_id = active_call_id_;
  if (call_id.empty() && life) {
    call_id = life->ActiveCallId();
  }
  if (call_id.empty()) {
    // Stale End button after Idle — force-clear chrome so Samsung does not look hung.
    ClearInCall();
    ClearRing();
    SyncShellState();
    return;
  }
  // Stop SDL/media on UI before LeaveCall worker (which must not CallMediaEngine::Stop off-UI)
  // and before remounting away the Leave button.
  if (auto* calls = Calls()) {
    if (calls->Media().IsActive() && calls->Media().ActiveCallId() == call_id) {
      calls->Media().Stop();
    }
  }
  active_call_id_.clear();
  ClearInCall();
  ClearRing();
  SyncShellState();
  if (life) {
    life->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  }
}

void CallController::RetryConnect() {
  BindToMessaging();
  auto* life = Lifecycle();
  std::string call_id = active_call_id_;
  if (call_id.empty() && life) {
    call_id = life->ActiveCallId();
  }
  if (call_id.empty() || !life) {
    return;
  }
  life->Apply(CallLifecycleEvent::RetryClicked, call_id);
  if (!life->LastError().empty()) {
    UserFeedback::Fail(life->LastError());
    life->ClearLastError();
  }
  RefreshPendingRing();
}

void CallController::ToggleMute() {
  BindToMessaging();
  auto* calls = Calls();
  if (!calls || !calls->Media().IsActive()) {
    return;
  }
  const bool before = calls->Media().IsMuted();
  if (auto muted = calls->SetLocalAudioMuted(!before); !muted) {
    UserFeedback::Fail(muted.error().message);
  }
  RefreshPendingRing();
}

void CallController::ToggleCamera() {
  BindToMessaging();
  auto* calls = Calls();
  if (!calls || !calls->Media().IsActive()) {
    return;
  }
  const bool next = !calls->Media().IsCameraEnabled();
  if (auto cam = calls->SetLocalVideoEnabled(next); !cam) {
    UserFeedback::Fail(cam.error().message);
  }
  RefreshPendingRing();
}

void CallController::ToggleSpeaker() {
  if (!CallAudioSession::SupportsSpeakerToggle()) {
    return;
  }
  BindToMessaging();
  auto* calls = Calls();
  if (!calls || !calls->Media().IsActive()) {
    return;
  }
  const bool before = CallAudioSession::IsSpeakerphoneOn();
  CallAudioSession::SetSpeakerphoneOn(!before);
  RefreshPendingRing();
}

void CallController::ApplyAudioLevels(CallMediaEngine& media) {
  media.RefreshRemoteVideoHealth();

  if (!shell_call_chrome_.call_in_progress) {
    return;
  }
  auto& in_call = shell_call_chrome_.call_in_progress();
  const bool muted = media.IsMuted();
  in_call.muted = muted;
  in_call.camera_on = media.IsCameraEnabled();
  in_call.show_speaker = CallAudioSession::SupportsSpeakerToggle();
  in_call.speaker_on = CallAudioSession::IsSpeakerphoneOn();

  bool peer_camera_on = false;
  bool have_peer_video_flag = false;
  auto* calls = Calls();
  if (calls && !active_call_id_.empty()) {
    if (auto peer_video = calls->PeerVideoEnabledForCall(active_call_id_);
        peer_video && peer_video->has_value()) {
      peer_camera_on = **peer_video;
      have_peer_video_flag = true;
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
  const bool p2p_failed =
      calls && calls->IsP2pConnectFailed() && !calls->IsAwaitingSfuRecovery() && !media.IsSfuMode();
  const bool media_reconnect =
      !p2p_failed && (stalling || missing_after_video || conn == "disconnected");

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

  in_call.stage_visible = in_call.camera_on || in_call.remote_video || in_call.local_preview ||
                          (have_peer_video_flag && peer_camera_on) || media_reconnect;
  if (in_call.remote_video) {
    in_call.remote_placeholder = "";
  } else if (have_peer_video_flag && !peer_camera_on) {
    in_call.remote_placeholder = Tr("call.placeholder.camera_off").c_str();
  } else if (p2p_failed) {
    in_call.remote_placeholder = Tr("call.status.couldnt_connect").c_str();
  } else if (media_reconnect) {
    in_call.remote_placeholder = Tr("call.status.reconnecting").c_str();
  } else {
    in_call.remote_placeholder = "";
  }

  in_call.mic_level = muted ? 0 : QuantizeAudioLevel(media.LocalInputLevel());
  in_call.peer_level = QuantizeAudioLevel(media.RemoteOutputLevel());
  in_call.mic_hint = LevelHint(in_call.mic_level, false, muted).c_str();
  in_call.peer_hint = LevelHint(in_call.peer_level, true, false).c_str();
  if (p2p_failed) {
    in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
  } else if (media_reconnect && !media.IsConnected()) {
    in_call.subtitle = Tr("call.status.reconnecting").c_str();
  } else if (stalling) {
    in_call.subtitle = Tr("call.status.reconnecting").c_str();
  } else if (media.IsConnected() ||
             (Lifecycle() && Lifecycle()->Phase() == CallPhase::InCall && media.IsActive())) {
    in_call.elapsed = FormatElapsed(media.ConnectedAtMs());
    if (!in_call.elapsed.empty()) {
      in_call.subtitle = in_call.elapsed;
    } else {
      in_call.subtitle = Tr("call.status.connected").c_str();
    }
  }

  ApplyMediaHealth(media, calls, media_reconnect || p2p_failed);
}

CallMediaHealthView CallController::BuildMediaHealthView(CallMediaEngine& media, CallSessionManager* calls,
                                                         const bool media_reconnect) const {
  CallMediaHealthInput in;
  in.engine = media.HealthSnapshot();
  if (calls) {
    in.hop = calls->HopHealth();
  }
  in.now_ms = util::NowUnixMs();
  in.reconnecting = media_reconnect;
  return EvaluateCallMediaHealth(in);
}

void CallController::ApplyMediaHealth(CallMediaEngine& media, CallSessionManager* calls,
                                      const bool media_reconnect) {
  if (!shell_call_chrome_.call_in_progress) {
    return;
  }
  auto& in_call = shell_call_chrome_.call_in_progress();
  if (!in_call.active) {
    return;
  }

  const CallMediaHealthView view = BuildMediaHealthView(media, calls, media_reconnect);
  const int64_t now_ms = util::NowUnixMs();

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

  const int q = static_cast<int>(view.quality);
  if (q != last_warned_quality_ &&
      (view.quality == CallPathQuality::NoAudio || view.quality == CallPathQuality::Poor ||
       view.asymmetry != CallAudioAsymmetry::None)) {
    last_warned_quality_ = q;
    if (const char* hint_key = CallAudioAsymmetryHintKey(view.asymmetry); hint_key && hint_key[0]) {
      UserFeedback::Fail(Tr(hint_key));
    } else if (const char* label_key = CallPathQualityLabelKey(view.quality); label_key && label_key[0]) {
      UserFeedback::Fail(Tr(label_key));
    }
  } else if (view.quality <= CallPathQuality::Good && view.asymmetry == CallAudioAsymmetry::None) {
    last_warned_quality_ = q;
  }
}

void CallController::ShowCallDetails() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { ShowCallDetails(); });
    return;
  }
  BindToMessaging();
  auto* calls = Calls();
  if (!calls || !shell_call_chrome_.call_in_progress) {
    return;
  }
  auto& in_call = shell_call_chrome_.call_in_progress();
  if (!in_call.active) {
    return;
  }

  auto& media = calls->Media();
  const bool media_reconnect = !media.IsConnected() && media.IsActive();
  const CallMediaHealthView view = BuildMediaHealthView(media, calls, media_reconnect);
  const int64_t now_ms = util::NowUnixMs();
  const bool diagnostics =
      call_ports_.call_diagnostics_enabled && call_ports_.call_diagnostics_enabled();

  CallDetailsCopy copy;
  copy.elapsed = in_call.elapsed.empty() ? std::string(in_call.subtitle.c_str())
                                         : std::string(in_call.elapsed.c_str());
  copy.path_label = view.path_kind == "relay" ? Tr("call.details.path.relay")
                                             : Tr("call.details.path.direct");
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
        if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
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
  auto* calls = Calls();
  if (!calls) {
    return;
  }
  // Keep Calling… / timer / levels fresh even before media starts.
  RefreshPendingRing();
}

} // namespace pbr
