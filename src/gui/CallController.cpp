#include <stdexcept>
#include "gui/CallController.h"

#include <chrono>

#include "foundation/i18n/LocalizationService.h"
#include "lib/media/AudioSession.h"
#include "lib/calls/CallMediaAdaptation.h"
#include "lib/media/IMediaView.h"
#include "lib/media/CameraCaptureOrientation.h"
#include "lib/media/DeviceVitals.h"
#include "lib/media/ThreadCpu.h"
#include "gui/UiFrameStats.h"
#include "common/media/CallMediaHealth.h"
#include "lib/calls/CallTypes.h"
#include "lib/calls/CallHopAttachLogic.h"
#include "lib/calls/CallScreen.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/people/ContactTypes.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/FramePacing.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/CallSystemUi.h"
#include "foundation/platform/IAssetLocator.h"
#include "foundation/platform/OngoingCallService.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/PendingNotificationTap.h"
#include "gui/CallStatusHint.h"
#include "lib/ui/ShellTypes.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "feature/calls/CallUiBackend.h"
#include "gui/CallChromeSync.h"
#include "gui/CallDemoScenes.h"
#include "gui/chat/MessageTimeLabel.h"
#include "foundation/product/DevTools.h"
#include "gui/CallMetricsTracker.h"
#include "lib/ui/CallConflictCopy.h"
#include "lib/calls/CallMediaErrorText.h"
#include "lib/ui/PaymentFeedback.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "CallVideoTileRenderer.h"
#include "gui/UserFeedback.h"

#include "lib/pricing/PricingTypes.h"

#include "common/Utilities.h"

#include <ui/Core.h>
#include <ui/base/SystemInterface.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

CallController::CallController()
    : system_call_screen_(
          ICallSystemUi::Instance(),
          {
              .accept = [this](const std::string& call_id,
                               const bool voice_only) { (void)presenter_.Accept(voice_only, call_id); },
              .decline = [this]() { (void)presenter_.Decline(); },
              .leave = [this]() { (void)presenter_.Leave(); },
              .toggle_mute = [this]() { ToggleMute(); },
              .display_name = [this](const std::string& identity) { return DisplayNameForIdentity(identity); },
              .now_ms = {},
          }) {
  redirectLogger("CallController");
  CallRingtone::SetRingSoundPath(IAssetLocator::Instance().Resolve("sounds/call_ring.wav"));
  CallPresenter::Callbacks callbacks;
  callbacks.on_changed = [this]() { Render(); };
  callbacks.notify = [this](const CallNotice& notice) { ShowNotice(notice); };
  callbacks.open_group_picker = [this](const std::string& thread_id, const bool video) {
    OpenGroupCallPicker(thread_id, video);
  };
  callbacks.open_add_people = [this](const std::string& call_id) {
    if (people_picker_notify_.open_for_call_add_guest) {
      people_picker_notify_.open_for_call_add_guest(call_id);
    }
  };
  callbacks.open_make_group = [this](const std::string& call_id) {
    if (people_picker_notify_.open_for_call_make_group) {
      people_picker_notify_.open_for_call_make_group(call_id);
    }
  };
  callbacks.on_ring_started = [this](const std::string& call_id, const bool video) {
    const int64_t now = util::NowUnixMs();
    EmitMetrics(metrics_.NoteRing(call_id, video, now, Vitals(now)));
  };
  callbacks.on_outbound_started = [this](const std::string& call_id, const bool video) {
    const int64_t now = util::NowUnixMs();
    EmitMetrics(metrics_.NoteOutbound(call_id, video, now, Vitals(now)));
  };
  callbacks.on_accept = [this](const std::string& call_id, const bool voice_only) {
    metrics_.NoteAccept(call_id, voice_only, util::NowUnixMs());
  };
  callbacks.on_decline = [this](const std::string& call_id) { metrics_.NoteDecline(call_id); };
  callbacks.on_call_over = [this](const std::string& call_id) { system_call_screen_.OnCallOver(call_id); };
  presenter_.BindCallbacks(std::move(callbacks));
  // Mobile: in the background the OS stops the camera; the call turns it off and says so (video_paused).
  background_listener_ = AppLifecycle::AddBackgroundListener([this]() { OnAppBackground(); });
  foreground_listener_ = AppLifecycle::AddForegroundListener([this]() { OnAppForeground(); });
  OngoingCallService::SetHangUpHandler([this]() {
    if (!presenter_.ActiveCallId().empty()) {
      log().info << "hang up from the ongoing-call notification";
      (void)presenter_.Leave();
    }
  });
}

CallController::~CallController() {
  OngoingCallService::SetHangUpHandler({});
  AppLifecycle::RemoveListener(background_listener_);
  AppLifecycle::RemoveListener(foreground_listener_);
}

void CallController::SyncOngoingCall() {
  const bool on = !presenter_.ActiveCallId().empty();
  const std::string title = on ? std::string(in_call_.phone_title.c_str()) : std::string();
  if (on == ongoing_call_on_ && title == ongoing_call_title_) {
    return;
  }
  ongoing_call_on_ = on;
  ongoing_call_title_ = title;
  OngoingCallService::SetOngoingCall(on, title);
}

void CallController::SyncLocalHold() {
  auto* backend = Backend();
  const bool in_call = backend && backend->Available() && !presenter_.ActiveCallId().empty();
  const bool held = in_call && AudioSession::IsInterrupted();
  if (held && local_on_hold_) {
    // SetLocalAudioMuted posts to the sessions thread: only once the hold's mute has been seen applied does an open
    // mic mean the user unmuted (by any path); before that it is just the post in flight (code review 2026-11-06).
    if (backend->Media().IsMuted()) {
      hold_mute_seen_applied_ = true;
    } else if (hold_mute_seen_applied_) {
      hold_mute_.UserToggled();
    }
  }
  if (held == local_on_hold_) {
    return;
  }
  local_on_hold_ = held;
  hold_mute_seen_applied_ = false;
  if (held) {
    // The other call rings as a banner on iPhones, so the user still sees PP: the long pill says what PP did; the
    // status line keeps saying it meanwhile (Kenneth 2026-11-06: the pill is the one style for every notice).
    UserFeedback::Note(Tr("call.status.on_hold_local"));
  }
  log().info << "call on hold=" << (held ? 1 : 0) << " (the OS " << (held ? "took" : "gave back") << " the audio)";
  if (in_call) {
    backend->SetLocalOnHold(held);
  }
  // Privacy: nothing of the other call may reach this one through our mic; a mute the user made before stays.
  if (const auto mute = hold_mute_.Next(held, in_call && backend->Media().IsMuted()); mute && in_call) {
    backend->SetLocalAudioMuted(*mute);
    if (!*mute) {
      UserFeedback::Note(Tr("call.status.on_hold_ended"));  // the hold opened the mic again: say so
    }
  }
  // The camera rests meanwhile, as in the background; still in the background at the end, OnAppForeground brings it.
  if (held && !video_paused_ && backend->Media().IsCameraEnabled()) {
    video_paused_ = true;
    backend->SetLocalVideoPaused(true);
  } else if (!held && video_paused_ && AppLifecycle::IsForeground()) {
    video_paused_ = false;
    if (backend && backend->Available()) {
      backend->SetLocalVideoPaused(false);
    }
  }
}

void CallController::OnAppBackground() {
  auto* backend = Backend();
  if (!Platform::IsMobile() || !backend || !backend->Available() || presenter_.ActiveCallId().empty() ||
      !backend->Media().IsCameraEnabled()) {
    return;
  }
  video_paused_ = true;
  log().info << "camera paused: app in the background";
  backend->SetLocalVideoPaused(true);
}

void CallController::OnAppForeground() {
  if (!video_paused_ || local_on_hold_) {  // on hold: the camera comes back when the hold ends (SyncLocalHold)
    return;
  }
  video_paused_ = false;
  if (auto* backend = Backend(); backend && backend->Available()) {
    log().info << "camera back: app in front";
    backend->SetLocalVideoPaused(false);
  }
}

namespace {

/** Remote frames this fresh mean the peer's camera is on, whatever its roster row says (B58). */
constexpr int64_t kPeerVideoLiveMs = 500;
/** A video call shows "Starting video…" at most this long while it waits for the first frame. */
constexpr int64_t kVideoStartMs = 10'000;

/** One key for an avatar written into the call page's markup: a change remounts it. */
std::string AvatarKey(const bool group, const bool has_avatar, const ui::String& src, const ui::String& letter,
                      const int tone) {
  return std::string(group ? "g" : "p") + (has_avatar ? std::string(src.c_str()) : std::string(letter.c_str())) +
         "|" + std::to_string(tone);
}

std::string RosterKey(const std::vector<CallRosterParticipantState>& roster) {
  std::string key;
  for (const CallRosterParticipantState& p : roster) {
    key += std::string(p.name.c_str()) + "|" + p.stream_id.c_str() + "|" + (p.audio_muted ? "m" : "-") +
           (p.video_enabled ? "v" : "-") + (p.has_remote_video ? "r" : "-") + (p.on_hold ? "h" : "-") +
           (p.video_paused ? "p" : "-") + (p.guest ? "g" : "-") +
           AvatarKey(false, p.has_avatar, p.avatar_src, p.avatar_letter, p.avatar_tone) + ";";
  }
  return key;
}

/** The rows whose speaking ring is on, in one key (kept out of RosterKey: a ring change dirties only the rows). */
std::string SpeakingKey(const std::vector<CallRosterParticipantState>& roster) {
  std::string key;
  for (const CallRosterParticipantState& p : roster) {
    if (p.speaking) {
      key += std::string(p.stream_id.c_str()) + ";";
    }
  }
  return key;
}

/** V062: the guest rows' words and buttons in one key (they are written into the markup: a change remounts). */
std::string GuestActionsKey(const std::vector<CallGuestActionState>& rows) {
  std::string key;
  for (const CallGuestActionState& row : rows) {
    key += std::string(row.identity.c_str()) + "|" + row.line.c_str() + "|" + row.add_to_group_label.c_str() + "|" +
           (row.can_remove ? "r" : "-") + ";";
  }
  return key;
}

CallChromeLayer CaptureCallChrome(const CallRingState& ring, const CallInProgressState& in_call, const bool phone) {
  return {
      .ring_active = ring.active,
      .in_call_active = in_call.active,
      .ring_pulse = ring.pulse,
      .in_call_muted = in_call.muted,
      .in_call_camera_on = in_call.camera_on,
      .in_call_camera_front = in_call.camera_front,
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
      .ring_guest_hint = ring.guest_hint.c_str(),
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
      .in_call_show_make_group = in_call.show_make_group,
      .in_call_group_invite = in_call.group_invite_text.c_str(),
      .in_call_guests = GuestActionsKey(in_call.guest_actions),
      .in_call_show_retry = in_call.show_retry,
      .in_call_calling = in_call.calling,
      .in_call_show_speaker = in_call.show_speaker,
      .in_call_show_camera = in_call.show_camera,
      .in_call_show_flip = in_call.show_flip,
      .in_call_participant_count = in_call.participant_count,
      .in_call_status_hint = in_call.status_hint.c_str(),
      .in_call_mode = in_call.mode,
      .in_call_spotlight = in_call.spotlight_stream.c_str(),
      .in_call_spotlight_fill = in_call.spotlight_fill,
      .in_call_minimized_side = in_call.minimized_side,
      .in_call_minimized_y_permille = in_call.minimized_y_permille,
      .in_call_minimized_tucked = in_call.minimized_tucked,
      .in_call_quality_bars = in_call.quality_bars,
      .in_call_quality_ok = in_call.quality_ok,
      .in_call_quality_warn = in_call.quality_warn,
      .in_call_quality_error = in_call.quality_error,
      .in_call_quality_label = in_call.quality_label.c_str(),
      .in_call_quality_hint = in_call.quality_hint.c_str(),
      .in_call_show_debug_subtitle = in_call.show_debug_subtitle,
      .in_call_debug_subtitle = in_call.debug_subtitle.c_str(),
      .in_call_roster = RosterKey(in_call.roster),
      .in_call_speaking = phone ? SpeakingKey(in_call.roster) : std::string{}, // only the phone page rings
      .phone = phone,
      .ring_phone_title = ring.phone_title.c_str(),
      .ring_phone_subtitle = ring.phone_subtitle.c_str(),
      .ring_avatar = AvatarKey(ring.group, ring.has_avatar, ring.avatar_src, ring.avatar_letter, ring.avatar_tone),
      .in_call_phone_title = in_call.phone_title.c_str(),
      .in_call_video = in_call.video,
      .in_call_avatars = AvatarKey(false, in_call.peer_has_avatar, in_call.peer_avatar_src, in_call.peer_avatar_letter,
                                   in_call.peer_avatar_tone) +
                         AvatarKey(false, in_call.self_has_avatar, in_call.self_avatar_src, in_call.self_avatar_letter,
                                   in_call.self_avatar_tone),
      .in_call_phone_failed = in_call.phone_failed,
      .in_call_failed_text = in_call.failed_text.c_str(),
      .in_call_story_active = in_call.story_active,
      .in_call_story_stage = in_call.story_stage,
      .in_call_story_steps = std::string(in_call.story_step1.c_str()) + "|" + in_call.story_step2.c_str() + "|" +
                             in_call.story_step3.c_str() + std::string("|") + in_call.story_step4.c_str() + "|" +
                             in_call.story_step5.c_str() + "|" + std::to_string(in_call.story_step_count),
      .in_call_story_sheet_open = in_call.story_sheet_open,
      .in_call_story_lines = in_call.story_lines,
      .in_call_story_how = in_call.story_how.c_str(),
      .in_call_result_text = in_call.result_text.c_str(),
      .in_call_result_fading = in_call.result_fading,
      .in_call_signal_shown = in_call.signal_shown,
      .in_call_signal_bars = in_call.signal_bars,
      .in_call_signal_tone = in_call.signal_tone,
      .in_call_signal_word = in_call.signal_word.c_str(),
      .in_call_notice_text = in_call.notice_text.c_str(),
      .in_call_notice_warn = in_call.notice_warn,
      .in_call_notice_fading = in_call.notice_fading,
      .in_call_reconnect_text = in_call.reconnect_text.c_str(),
      .in_call_reconnect_ok = in_call.reconnect_ok,
      .in_call_reconnect_fading = in_call.reconnect_fading,
      .in_call_reconnect_dim = in_call.reconnect_dim,
  };
}

/** "1.8" from tenths of a second. */
std::string FormatTenths(const int64_t tenths) {
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
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

/** UI demo only: a stand-in camera picture (the simulator has none), a top-to-bottom blend of two colours. */
CallVideoTileRenderer::Frame DemoCameraFrame(const uint64_t seq, const uint8_t top[3], const uint8_t bottom[3]) {
  CallVideoTileRenderer::Frame frame;
  frame.width = 90;
  frame.height = 160;
  frame.seq = seq;
  frame.rgba.resize(static_cast<size_t>(frame.width) * frame.height * 4);
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      uint8_t* px = &frame.rgba[(static_cast<size_t>(y) * frame.width + x) * 4];
      for (int c = 0; c < 3; ++c) {
        px[c] = static_cast<uint8_t>(top[c] + (bottom[c] - top[c]) * y / (frame.height - 1));
      }
      px[3] = 255;
    }
  }
  return frame;
}


std::string ComposeP2pStatusHint(bool missing_mic, bool seed_unreachable) {
  std::string hint;
  for (const char* key : P2pStatusHintKeys(missing_mic, seed_unreachable)) {
    const std::string line = Tr(key);
    if (line.empty()) {
      continue;
    }
    if (!hint.empty()) {
      hint += " ";
    }
    hint += line;
  }
  return hint;
}

std::string ComposeGroupCallStatusHint() {
  return Tr("call.hint.group_media");
}

/** The call's media was live and is not now while it runs (it reconnects or moves path): the call's state. */
bool CallMediaDropped(CallUiBackend& backend, IMediaView& media, const std::string& call_id) {
  return media.IsActive() && !backend.MediaChromeLive() && backend.LiveSinceMs(call_id) > 0;
}

} // namespace

void CallController::BindCallPorts(CallFunctionalPorts ports) {
  call_ports_ = ports;
  presenter_.BindPorts(std::move(ports));
}

void CallController::BindShellCallChrome(ShellCallChromePorts ports) {
  shell_call_chrome_ = std::move(ports);
}

void CallController::BindPeerLinkRefresh(std::function<void()> callback) {
  peer_link_refresh_ = std::move(callback);
}

void CallController::BindOpenThread(std::function<void(const std::string& thread_id)> callback) {
  open_thread_ = std::move(callback);
}

void CallController::BindPeoplePickerNotify(PeoplePickerNotifyPorts ports) {
  people_picker_notify_ = std::move(ports);
}

CallUiBackend* CallController::Backend() {
  return presenter_.Backend();
}

void CallController::BindToMessaging() {
  presenter_.BindToMessaging();
}

void CallController::Tick() {
  if constexpr (kUiDemoBuild) {
    if (RenderDemoScene()) {
      return;  // the demo app's scripted call (PP_DEMO_CALL) stands in for the presenter
    }
  }
  presenter_.Tick();  // expired rings; the in-call screen (Calling… / timer / levels) every frame
  const int64_t now = util::NowUnixMs();
  if (ring_.active && !phone_layout_) {  // the phone ring page has no pulse: no dirty, no forced frame
    if (now - last_pulse_toggle_ms_ >= 600) {
      ring_.pulse = !ring_.pulse;
      last_pulse_toggle_ms_ = now;
      SyncShellState();
    }
  }
  SyncRingtone();
  SyncLocalHold();
  SyncOngoingCall();
  EndDisconnectedCall(now);
  if (metrics_.Tracking()) {
    auto* backend = Backend();
    const bool call_live = ring_.active || !presenter_.ActiveCallId().empty() ||
                           (backend && backend->Available() && backend->Phase() != CallPhase::Idle);
    EmitMetrics(metrics_.Tick(call_live, now, Vitals(now)));
    EmitPerfThreads(now);
  } else {
    perf_sample_ms_ = 0;
    perf_thread_cpu_.clear();
  }
  // UI-stuck probe only while a call is tracked (a thread posting a ping every 500 ms).
  if (metrics_.Tracking() && !ui_probe_.Running()) {
    ui_probe_.Start([](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); },
                    [this](int64_t latency_ms) { EmitMetrics(metrics_.NoteUiLatency(latency_ms)); });
  } else if (!metrics_.Tracking() && ui_probe_.Running()) {
    ui_probe_.Stop();
  }
}

void CallController::BindVideoFrameWake(IMediaView& media) {
  if (video_wake_bound_ == &media) {
    return;
  }
  video_wake_bound_ = &media;
  // Any thread (the camera's, a decoder's): a new frame to draw. Nothing else wakes the frame loop for it.
  media.SetOnVideoFrame([last = std::make_shared<std::atomic<int64_t>>(0)]() {
    if (TakeVideoFrameWake(*last, util::NowUnixMs())) {
      Backend::RequestForceFrame();
    }
  });
}

const DeviceVitals& CallController::Vitals(const int64_t now_ms) {
  // UIDevice / getrusage: every couple of seconds is plenty for power and thermal trends.
  if (vitals_read_ms_ == 0 || now_ms - vitals_read_ms_ >= 2000) {
    vitals_ = ReadDeviceVitals();
    vitals_read_ms_ = now_ms;
  }
  return vitals_;
}

void CallController::EmitPerfThreads(const int64_t now_ms) {
  constexpr int64_t kPerfIntervalMs = 10000;
  if (perf_sample_ms_ != 0 && now_ms - perf_sample_ms_ < kPerfIntervalMs) {
    return;
  }
  const DeviceVitals vitals = ReadDeviceVitals();
  const std::vector<ThreadCpuSample> threads = ReadThreadCpu();
  const auto top = TopThreadCpuDeltas(threads, perf_thread_cpu_, 8);
  const auto top_raw = TopThreadCpuDeltasByOwnName(threads, perf_thread_cpu_raw_, 12);
  const double prev_proc_cpu_s = perf_proc_cpu_s_;
  const int64_t prev_sample_ms = perf_sample_ms_;
  perf_proc_cpu_s_ = vitals.cpu_s;
  perf_sample_ms_ = now_ms;
  const UiFrameTotals frames = UiFrameStats::TakeAndReset();
  if (prev_sample_ms == 0) {
    return; // the first sample only sets the baseline
  }
  static constexpr ShareWord kKeys[] = {"t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8"};
  static constexpr ShareWord kMsKeys[] = {"t1_ms", "t2_ms", "t3_ms", "t4_ms", "t5_ms", "t6_ms", "t7_ms", "t8_ms"};
  MetricsLine line("perf.threads");
  line.Add("interval_ms", now_ms - prev_sample_ms)
      .Add("proc_cpu_ms", (vitals.cpu_s >= 0 && prev_proc_cpu_s >= 0) ? (vitals.cpu_s - prev_proc_cpu_s) * 1000.0 : -1.0)
      .Add("thermal", vitals.thermal)
      .Add("battery", vitals.battery_pct)
      .Add("charging", vitals.charging)
      .Add("video_on", in_call_.camera_on)
      .Add("remote_video", in_call_.remote_video)
      .Add("frames", frames.frames)
      .Add("layout_ms", static_cast<double>(frames.layout_us) / 1000.0)
      .Add("render_ms", static_cast<double>(frames.render_us) / 1000.0);
  for (size_t i = 0; i < top.size(); ++i) {
    line.Pick(kKeys[i], top[i].name, kKnownThreadNames).Add(kMsKeys[i], top[i].ms);
  }
  line.Emit();
  // Every thread by its own name (network, worker pools, system queues) — the developer log only, which is
  // never uploaded; the shareable line above folds unknown names into "other".
  std::ostringstream raw;
  raw << "perf.threads.raw interval_ms=" << (now_ms - prev_sample_ms) << " threads=" << threads.size();
  for (const ThreadCpuDelta& t : top_raw) {
    raw << ' ' << t.name << '=' << static_cast<int64_t>(t.ms + 0.5);
  }
  log().info << raw.str();
}

void CallController::EmitMetrics(const std::vector<ShareableLine>& lines) {
  for (const ShareableLine& line : lines) {
    line.Emit();
  }
}

void CallController::OnCallWake() {
  presenter_.OnCallWake();
}

void CallController::OnMessagingReady() {
  system_call_screen_.OnMessagingReady();
}

void CallController::ClearRing() {
  ring_ = {};
  ringtone_.Stop();
}

void CallController::PrepareForShutdown() {
  if (!AppRuntime::CurrentlyOnUI()) {
    log().warning << "PrepareForShutdown off UI thread";
  }
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
  chrome_mode_ = CallChromeMode::Expanded;
  restore_mode_ = CallChromeMode::Expanded;
  minimized_side_ = 0;
  minimized_y_permille_ = 0;
  minimized_tucked_ = false;
  chrome_mode_call_id_.clear();
  last_media_health_log_ms_ = 0;
  last_debug_subtitle_ = {};
  audio_fault_gate_.Reset();
  peer_video_log_key_.clear();
  video_start_since_ms_ = 0;
  camera_sync_off_done_ = false;
  story_.Reset();
  reconnect_ = CallReconnectCapsule{};
  reconnecting_now_ = false;
  story_sheet_open_ = false;
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
  in_call.minimized_side = minimized_side_;
  in_call.minimized_y_permille = minimized_y_permille_;
  in_call.minimized_tucked = minimized_tucked_;
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

void CallController::ApplySpotlightToState(CallInProgressState& in_call) {
  const std::string spotlit =
      ResolveCallSpotlight(in_call.roster, in_call.show_roster, spotlight_choice_, active_speaker_.Current());
  if (!spotlight_choice_.stream.empty() && spotlit != spotlight_choice_.stream) {
    spotlight_choice_.stream.clear(); // they left the call
  }
  in_call.spotlight_stream = spotlit.c_str();
  in_call.spotlight_fill = spotlight_fill_;
  for (CallRosterParticipantState& row : in_call.roster) {
    row.speaking =
        std::binary_search(speaking_shown_.begin(), speaking_shown_.end(), std::string(row.stream_id.c_str()));
  }
  in_call.spotlight_rows.clear();
  in_call.strip_rows.clear();
  if (!spotlit.empty()) {
    for (const CallRosterParticipantState& row : in_call.roster) {
      (spotlit == row.stream_id.c_str() ? in_call.spotlight_rows : in_call.strip_rows).push_back(row);
    }
  }
}

void CallController::SpotlightParticipant(const std::string& stream_id) {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this, stream_id]() { SpotlightParticipant(stream_id); });
    return;
  }
  spotlight_choice_.stream = stream_id;
  spotlight_choice_.grid = false;
  spotlight_fill_ = true;
  ResyncSpotlight();
}

void CallController::SpotlightGrid() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { SpotlightGrid(); });
    return;
  }
  spotlight_choice_ = {.grid = true};
  ResyncSpotlight();
}

void CallController::ToggleSpotlightFill() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { ToggleSpotlightFill(); });
    return;
  }
  spotlight_fill_ = !spotlight_fill_;
  ResyncSpotlight();
}

void CallController::NoteActiveSpeaker(const MediaEngineHealth& engine, const float self_level,
                                       const int64_t now_ms) {
  if (!in_call_.show_roster) {
    return; // 1:1: the peer is spotlit anyway
  }
  std::vector<std::pair<std::string, float>> levels;
  levels.reserve(engine.streams.size());
  for (const MediaStreamHealth& stream : engine.streams) {
    levels.emplace_back(std::to_string(stream.stream_id), stream.peak_level);
  }
  const std::string before = active_speaker_.Current();
  const bool speaker_moved = active_speaker_.Update(levels, now_ms) != before;

  // The speaking rings: the others by their streams, we by our microphone (never the spotlight: that stays the
  // others'). A change shows at most every kSpeakingRingRefreshMs; it dirties the rows only (CallChromeSync).
  if (self_level >= CallActiveSpeaker::kSpeakingLevel) {
    self_heard_ms_ = now_ms;
  }
  std::vector<std::string> speaking = active_speaker_.Speaking(now_ms);
  if (self_heard_ms_ != 0 && now_ms - self_heard_ms_ <= CallActiveSpeaker::kSpeakingRingMs) {
    for (const CallRosterParticipantState& row : in_call_.roster) {
      if (row.is_local) {
        speaking.emplace_back(row.stream_id.c_str());
      }
    }
    std::sort(speaking.begin(), speaking.end());
  }
  bool rings_changed = false;
  if (speaking != speaking_shown_ && now_ms - speaking_shown_ms_ >= kSpeakingRingRefreshMs) {
    speaking_shown_ = std::move(speaking);
    speaking_shown_ms_ = now_ms;
    rings_changed = true;
  }
  if (speaker_moved) {
    ResyncSpotlight();
  } else if (rings_changed) {
    ApplySpotlightToState(in_call_);  // the render's SyncShellState pushes it
  }
}

void CallController::ResyncSpotlight() {
  if (in_call_.active) {
    ApplySpotlightToState(in_call_);
    SyncShellState();
  }
}

void CallController::ImmersiveChrome() {
  SetChromeMode(CallChromeMode::Immersive);
}

void CallController::RestoreChromeFromMinimized() {
  SetChromeMode(restore_mode_ == CallChromeMode::Immersive ? CallChromeMode::Immersive
                                                          : CallChromeMode::Expanded);
}

void CallController::SetMinimizedPlace(int side, int y_permille, bool tucked) {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this, side, y_permille, tucked]() { SetMinimizedPlace(side, y_permille, tucked); });
    return;
  }
  const int clamped_side = std::clamp(side, 0, 2);
  const int clamped_y = clamped_side == 0 ? 0 : std::clamp(y_permille, 0, 1000);
  const bool clamped_tucked = tucked && clamped_side != 0;
  if (minimized_side_ == clamped_side && minimized_y_permille_ == clamped_y && minimized_tucked_ == clamped_tucked) {
    return;
  }
  minimized_side_ = clamped_side;
  minimized_y_permille_ = clamped_y;
  minimized_tucked_ = clamped_tucked;
  if (in_call_.active) {
    in_call_.minimized_side = minimized_side_;
    in_call_.minimized_y_permille = minimized_y_permille_;
    in_call_.minimized_tucked = minimized_tucked_;
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
  const CallChromeLayer next = CaptureCallChrome(ring_, in_call_, phone_layout_);
  const CallChromeUpdate update = chrome_gate_.Decide(next, std::chrono::steady_clock::now());

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
  // The phone's own call screen rings it: the app's stays quiet.
  const bool should_ring = ring_.active && !system_call_screen_.SystemRings(presenter_.Screen().ring_call_id);
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
  // against the worker's own ActivateForVoip() and leave the phone in VoIP mode.
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
  if (auto contact = call_ports_.find_contact_by_identity(identity, mesh::ContactIdKind::Account)) {
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
  return FormatMinSec((util::NowUnixMs() - connected_at_ms) / 1000);
}

std::string CallController::FormatMinSec(const int64_t seconds) {
  const int64_t sec = std::max<int64_t>(0, seconds);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(sec / 60), static_cast<long long>(sec % 60));
  return buf;
}

void CallController::RefreshPendingRing() {
  presenter_.Refresh();
}

// A notice's words for the user: its locale key when the feature gave one, else transport text
// ("amp call-media: no path within the reconnect window") mapped to a plain sentence and logged
// raw (B89); payment errors keep their own mapping.
std::string CallController::NoticeUserText(const CallNotice& notice) const {
  if (!notice.key.empty()) {
    return Tr(notice.key);
  }
  if (const char* key = CallMediaErrorUserKey(notice.text)) {
    log().info << "call media error shown as " << key << " raw=" << notice.text;
    return Tr(key);
  }
  return PaymentErrorUserMessage(notice.text);
}

void CallController::ShowNotice(const CallNotice& notice) {
  switch (notice.kind) {
  case CallNotice::Kind::Error:
    UserFeedback::Fail(NoticeUserText(notice));
    return;
  case CallNotice::Kind::StickyError:
    // SoftMigrate / hop failures need a sticky banner — toast (even Long=6s) vanishes before
    // users can read multi-hop diagnostics.
    UserFeedback::NeedsSetup(NoticeUserText(notice), /*sticky=*/true);
    return;
  case CallNotice::Kind::PeerEnded:
  case CallNotice::Kind::PeerDeclined: {
    const bool declined = notice.kind == CallNotice::Kind::PeerDeclined;
    const std::string name = DisplayNameForIdentity(notice.identity);
    const char* named = declined ? "call.status.declined_by_peer" : "call.status.ended_by_peer";
    const char* unknown = declined ? "call.status.declined_by_peer_unknown" : "call.status.ended_by_peer_unknown";
    UserFeedback::Ok(name.empty() ? Tr(unknown) : Tr(named, {{"name", name}}));
    return;
  }
  case CallNotice::Kind::GuestJoined: {
    std::string name = DisplayNameForIdentity(notice.identity);
    if (name.empty()) {
      name = notice.identity;
    }
    const std::string local = call_ports_.local_relay_identity ? call_ports_.local_relay_identity().value_or("") : "";
    if (!local.empty() && notice.brought_by == local) {
      UserFeedback::Ok(Tr("call.guest.joined_yours", {{"name", name}}));
      return;
    }
    std::string bringer = DisplayNameForIdentity(notice.brought_by);
    if (bringer.empty()) {
      bringer = notice.brought_by;
    }
    UserFeedback::Ok(Tr("call.guest.joined", {{"name", name}, {"bringer", bringer}}));
    return;
  }
  case CallNotice::Kind::IncomingCall: {
    if (system_call_screen_.TakesRing(AppLifecycle::IsVisible(), notice.conflict)) {
      return; // the phone's own call screen rings it (Android's call notification)
    }
    const std::string caller = DisplayNameForIdentity(notice.identity);
    if (notice.conflict) {
      // V064: over a call, the alert says who is calling and which call answering ends, as the ring does.
      const CallConflictCopy copy = MakeCallConflictCopy(true, ConflictNames(caller, true, notice.ending));
      ILocalNotifier::Instance().NotifyIncoming(copy.title, copy.hint, "");
      return;
    }
    const std::string body =
        caller.empty() ? Tr("call.notify.body_unknown") : Tr("call.notify.body_named", {{"name", caller}});
    ILocalNotifier::Instance().NotifyIncoming(Tr("call.notify.title"), body, "");
    return;
  }
  case CallNotice::Kind::CallBack:
    OfferCallBack(notice.ending);
    return;
  }
}

void CallController::OfferCallBack(const CallScreen::EndingCall& ended) {
  std::string name = DisplayNameForIdentity(ended.peer_identity);
  if (name.empty()) {
    name = Tr("call.label.them");
  }
  const std::string thread_id = ended.thread_id;
  const bool video = ended.video;
  UserFeedback::Confirm(
      Tr("call.call_back.title", {{"name", name}}), Tr("call.call_back.body", {{"name", name}}),
      [this, thread_id, video](const bool call) {
        if (call) {
          (void)StartCall(thread_id, video);
        }
      },
      Tr("call.call_back.action"));
}

void CallController::Render() {
  phone_layout_ = shell_call_chrome_.phone_layout && shell_call_chrome_.phone_layout();
  const CallScreen& screen = presenter_.Screen();
  // Visible, not foreground: an overlay (the system's own call screen, Control Center) makes iOS resign the app
  // active, which is not a reason to move a ring to the system screen.
  system_call_screen_.Sync(screen, presenter_.ActiveCallId(), AppLifecycle::IsVisible());
  // The chat header shows "In call": tell it when the call on screen changes.
  if (screen.kind != header_kind_ || presenter_.ActiveCallId() != header_call_id_) {
    header_kind_ = screen.kind;
    header_call_id_ = presenter_.ActiveCallId();
    if (peer_link_refresh_) {
      peer_link_refresh_();
    }
  }
  switch (screen.kind) {
  case CallScreen::Kind::Answering:
    RenderAnswering(screen);
    return;
  case CallScreen::Kind::Ringing:
    if (system_call_screen_.SystemRings(screen.ring_call_id)) {
      // The phone's own call screen rings it (Android's call notification, P014): one ring, not the app's dialog too.
      ClearRing();
      SyncShellState();
      SyncRingtone();
    } else {
      RenderRing(screen);
    }
    // The ring of a tapped call alert (its call id, or the first ring for a push without one): land on its chat
    // once; a stale tap expires unused.
    if (open_thread_ && !screen.thread_id.empty() &&
        PendingNotificationTap::Instance().TakeCallRoute(screen.ring_call_id, PendingNotificationTap::Clock::now())) {
      AppRuntime::PostUI([this, thread_id = screen.thread_id]() { open_thread_(thread_id); });
    }
    return;
  case CallScreen::Kind::InCall:
    if (auto* backend = Backend(); backend && backend->Available()) {
      RenderInCall(screen, *backend);
    }
    return;
  case CallScreen::Kind::None:
    if (!presenter_.ActiveCallId().empty()) {
      // Still in a call (a ring over it was answered or declined): only the ring goes; the call's
      // chrome mode stays as the user left it.
      ClearRing();
      SyncShellState();
      return;
    }
    ClearInCall();
    ClearRing();
    SyncShellState();
    return;
  }
}

void CallController::RenderAnswering(const CallScreen& screen) {
  // Accept in flight (CALLS.md): the ring is gone; a Connecting bar so Accept does not look hung.
  ClearRing();
  in_call_.active = true;
  in_call_.call_id = screen.ring_call_id;
  in_call_.subtitle = Tr("call.status.connecting").c_str();
  in_call_.title = screen.ring_video ? Tr("call.title.video").c_str() : Tr("call.title.voice").c_str();
  const std::string caller = DisplayNameForIdentity(screen.caller_identity);
  in_call_.peer_label = caller.empty() ? screen.caller_identity.c_str() : caller.c_str();
  // Answered with video: switch to the video stage right away (see kVideoStartMs).
  in_call_.stage_visible = screen.answered_video;
  in_call_.remote_video = false;
  in_call_.remote_placeholder = screen.answered_video ? Tr("call.placeholder.starting_video").c_str() : "";
  in_call_.show_speaker = screen.show_speaker;
  in_call_.speaker_on = screen.speaker_on;
  if (phone_layout_) {
    // The phone page tells the answer's story from here ("You answered", setting up the encrypted line), in the
    // layout the call will keep: a group's grid from the start, so it does not flash the 1:1 page first. Desktop
    // keeps its Connecting bar as before.
    in_call_.show_roster = screen.ring_group;
    if (screen.ring_group) {
      in_call_.title = screen.ring_video ? Tr("call.title.group_video").c_str() : Tr("call.title.group_voice").c_str();
    }
    if (chrome_mode_call_id_ != screen.ring_call_id) {
      chrome_mode_ = DefaultCallChromeMode(true);
      restore_mode_ = chrome_mode_;
      chrome_mode_call_id_ = screen.ring_call_id;
    }
    ApplyChromeModeToState(in_call_);
    FillPhoneCallFields(screen);
    ApplyConnectStory(screen, std::string(), CallPathQuality::Good, 4, util::NowUnixMs());
  }
  SyncShellState();
  SyncRingtone();
}

void CallController::RenderRing(const CallScreen& screen) {
  std::string caller_label = DisplayNameForIdentity(screen.caller_identity);
  if (caller_label.empty()) {
    caller_label = screen.caller_identity;
  }
  const CallConflictCopy copy =
      MakeCallConflictCopy(screen.conflict, ConflictNames(caller_label, screen.conflict, screen.ending));

  // Hide the in-call bar while ringing a *different* call (the call itself stays active).
  HideInCallChrome();

  ring_.active = true;
  ring_.conflict = screen.conflict;
  ring_.call_id = screen.ring_call_id;
  ring_.caller_label = caller_label;
  ring_.video_allowed = screen.ring_video;
  // M6: headline just says what kind of call this is; the voice-answer button covers the choice. Over a call it
  // says who is calling (V064: the hint under it, which call answering ends).
  if (!copy.title.empty()) {
    ring_.media_label = copy.title;
  } else {
    ring_.media_label =
        screen.ring_video ? Tr("call.ring.incoming_video").c_str() : Tr("call.ring.incoming_voice").c_str();
  }
  ring_.eyebrow = copy.eyebrow;
  ring_.conflict_hint = copy.hint;
  // V062 5: brought in as a guest — joining shows them to everyone in the call.
  ring_.guest_hint =
      screen.ring_guest ? Tr("call.ring.guest_notice", {{"count", std::to_string(screen.ring_people)}}).c_str() : "";
  ring_.accept_label = copy.accept_label;
  ring_.decline_label = copy.decline_label;
  // V035: video ring offers a voice-answer alternative; the primary button reads
  // "Video answer" so it's clear Accept keeps the camera option (conflict wording unchanged).
  if (ring_.video_allowed && !screen.conflict) {
    ring_.accept_label = Tr("call.ring.accept_video").c_str();
  }

  // P001: show waive / take-all when inviter offered a positive initiation amount.
  ring_.show_pricing = screen.offer_minor > 0;
  ring_.accept_charge_enabled = screen.charge_enabled;
  if (ring_.show_pricing) {
    const std::string amount = std::to_string(screen.offer_minor);
    ring_.pricing_label =
        Tr("call.pricing.offer", {{"amount", amount}, {"currency", kPricingCurrencyDisplayName}}).c_str();
    ring_.accept_charge_label =
        Tr("call.ring.accept_charge", {{"amount", amount}, {"currency", kPricingCurrencyDisplayName}}).c_str();
    ring_.accept_charge_hint = Tr("call.ring.accept_charge_disabled_hint").c_str();
    ring_.accept_label =
        screen.conflict ? Tr("call.ring.end_and_accept_free").c_str() : Tr("call.ring.accept_free").c_str();
  } else {
    ring_.pricing_label.clear();
    ring_.accept_charge_label.clear();
    ring_.accept_charge_hint.clear();
  }

  // I2/M3: voice-answer button label — conflict wording wins over pricing when both apply.
  if (screen.conflict) {
    ring_.voice_answer_label = Tr("call.ring.end_and_accept_voice").c_str();
  } else if (ring_.show_pricing) {
    ring_.voice_answer_label = Tr("call.ring.voice_answer_free").c_str();
  } else {
    ring_.voice_answer_label = Tr("call.ring.accept_voice").c_str();
  }

  // Short labels for the round icon buttons (WeChat-style ring redesign) — same
  // conflict-wins-over-pricing precedence as the long labels above.
  if (screen.conflict) {
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
  FillRingPhoneFields(screen, caller_label);
  SyncShellState();
  SyncRingtone();
}

void CallController::RenderInCall(const CallScreen& screen, CallUiBackend& backend) {
  ClearRing();
  FillInCallFromScreen(screen);
  ApplyAudioLevels(backend.Media());
  {
    static std::string last_sub_log;
    const std::string sub = in_call_.subtitle.c_str();
    if (sub != last_sub_log) {
      last_sub_log = sub;
      log().info << "in-call subtitle=\"" << sub << "\" phase=" << CallPhaseName(backend.Phase())
                 << " status=" << CallMediaStatusName(backend.MediaStatus())
                 << " screen=" << CallScreenStatusName(screen.status)
                 << " media_live=" << (backend.MediaChromeLive() ? 1 : 0)
                 << " media_active=" << (backend.Media().IsActive() ? 1 : 0);
    }
  }
  SyncShellState();
}

void CallController::FillInCallFromScreen(const CallScreen& screen) {
  auto& in_call = in_call_;
  in_call.active = true;
  in_call.call_id = screen.call_id;
  in_call.muted = screen.muted;
  in_call.show_speaker = screen.show_speaker;
  in_call.speaker_on = screen.speaker_on;

  const int joined_count = static_cast<int>(screen.members.size());
  in_call.participant_count = joined_count;
  in_call.show_roster = screen.group;
  in_call.show_invite = screen.show_invite;
  in_call.show_make_group = screen.show_make_group;
  in_call.group_invite_text =
      screen.group_invite
          ? Tr("call.group_invite.banner", {{"name", DisplayNameForIdentity(screen.group_invite->maker_identity)},
                                            {"group", screen.group_invite->group_title}})
                .c_str()
          : "";
  FillGuestActions(in_call, screen);
  in_call.roster.clear();
  in_call.roster.reserve(screen.members.size());
  for (const CallScreen::Member& member : screen.members) {
    CallRosterParticipantState entry;
    entry.is_local = member.is_local;
    std::string name = member.is_local ? Tr("call.label.you") : DisplayNameForIdentity(member.identity);
    if (!member.is_local && name.empty()) {
      name = member.identity;
    }
    entry.name = name.c_str();
    entry.identity = member.identity.c_str();
    if (call_ports_.identity_icon_local_path) {
      const std::string avatar_path = call_ports_.identity_icon_local_path(member.identity);
      if (!avatar_path.empty()) {
        entry.has_avatar = true;
        entry.avatar_src = avatar_path.c_str();
      }
    }
    const AvatarGlyph glyph = MakeAvatarGlyph(name, member.identity);
    entry.avatar_letter = glyph.letter.c_str();
    entry.avatar_tone = glyph.tone;
    entry.stream_id = std::to_string(PublisherStreamIdForIdentity(member.identity)).c_str();
    entry.audio_muted = member.audio_muted;
    entry.video_enabled = member.video_enabled;
    entry.on_hold = member.on_hold;
    entry.video_paused = member.video_paused;
    entry.guest = member.guest;
    in_call.roster.push_back(std::move(entry));
  }

  // V031: pick default once per call; preserve user mode across ticks.
  if (chrome_mode_call_id_ != screen.call_id) {
    chrome_mode_ = DefaultCallChromeMode(in_call.show_roster || phone_layout_);  // phones: the full call page
    restore_mode_ = chrome_mode_;
    chrome_mode_call_id_ = screen.call_id;
    spotlight_choice_ = {};
    spotlight_fill_ = true;
    active_speaker_.Reset();
    speaking_shown_.clear();
    speaking_shown_ms_ = 0;
    self_heard_ms_ = 0;
  }
  ApplyChromeModeToState(in_call);
  ApplySpotlightToState(in_call);

  if (in_call.show_roster) {
    in_call.title = screen.video ? Tr("call.title.group_video").c_str() : Tr("call.title.group_voice").c_str();
  } else {
    in_call.title = screen.video ? Tr("call.title.video").c_str() : Tr("call.title.voice").c_str();
  }

  std::string peer_label = Tr("call.label.them");
  if (const std::string name = DisplayNameForIdentity(screen.peer_identity); !name.empty()) {
    peer_label = name;
  }
  in_call.peer_label = in_call.show_roster ? Tr("call.label.others").c_str() : peer_label.c_str();
  in_call.show_retry = screen.show_retry;
  in_call.calling = ShowsCancelCall(screen);

  // The status line (CallPresenter decides it: DecideInCallStatus).
  using Status = CallScreen::Status;
  in_call.elapsed = FormatElapsed(screen.connected_at_ms);
  in_call.status_hint = {};
  switch (screen.status) {
  case Status::CouldntConnect:
    in_call.subtitle = Tr("call.status.couldnt_connect").c_str();
    if (screen.hint == CallScreen::Hint::GroupMedia) {
      in_call.status_hint = ComposeGroupCallStatusHint().c_str();
    } else if (screen.hint == CallScreen::Hint::DirectConnect) {
      in_call.status_hint = ComposeP2pStatusHint(screen.hint_missing_mic, screen.hint_seed_unreachable).c_str();
    }
    break;
  case Status::Activity:
    in_call.subtitle = screen.activity.c_str();
    if (screen.activity == Tr("call.status.switching_media_path")) {
      in_call.status_hint = Tr("call.hint.switching_media_path").c_str();
    } else if (screen.activity == Tr("call.status.looking_for_another_path")) {
      in_call.status_hint = Tr("call.hint.looking_for_another_path").c_str();
    }
    break;
  case Status::Reconnecting:
    // k4: the call lost its path and waits for a new one; the call timer keeps running.
    in_call.subtitle = in_call.elapsed;  // greyed; the capsule says "Reconnecting…"
    break;
  case Status::WaitingForOthers: {
    // V053: everyone else left; counts down to leaving by itself.
    const std::string left = FormatMinSec((screen.alone_until_ms - util::NowUnixMs() + 999) / 1000);
    in_call.subtitle = Tr("call.status.waiting_for_others", {{"time", left}}).c_str();
    break;
  }
  case Status::Disconnected:
    in_call.subtitle = Tr("call.status.disconnected").c_str();
    break;
  case Status::SendingOnly:
    in_call.subtitle = Tr(media_hint_.SendingOnlyKey(in_call.video)).c_str();
    break;
  case Status::Connected:
    in_call.subtitle = in_call.elapsed.empty() ? Tr("call.status.connected").c_str() : in_call.elapsed;
    break;
  case Status::Calling:
    in_call.subtitle = Tr("call.status.calling").c_str();
    break;
  case Status::Connecting:
    in_call.subtitle = Tr("call.status.connecting").c_str();
    break;
  case Status::SettingUpGroup:
    in_call.subtitle = Tr("call.status.setting_up_group").c_str();
    break;
  case Status::WaitingForMediaPath:
    in_call.subtitle = Tr("call.status.waiting_for_media_path").c_str();
    break;
  }
  if (in_call.show_roster && ShowsParticipantCount(screen)) {
    in_call.subtitle = Tr("call.participants_elapsed",
                          {{"count", std::to_string(joined_count)}, {"elapsed", std::string(in_call.elapsed.c_str())}})
                           .c_str();
  }
  FillPhoneCallFields(screen);
}

bool CallController::StartCall(const std::string& thread_id, const bool video_allowed) {
  return presenter_.StartCall(thread_id, video_allowed);
}

bool CallController::StartCallWithInvitees(const std::string& thread_id, const bool video_allowed,
                                           const std::vector<std::string>& invitee_identities) {
  return presenter_.StartCallWithInvitees(thread_id, video_allowed, invitee_identities);
}

bool CallController::CallJoinableInThread(const std::string& thread_id) {
  return presenter_.CallJoinableInThread(thread_id);
}

std::string CallController::CallJoinBarText(const std::string& thread_id) {
  const CallJoinBar bar = presenter_.JoinBarInThread(thread_id);
  if (!bar.shown) {
    return {};
  }
  std::string text = Tr(bar.text_key);
  if (bar.joined > 0) {
    text += " · " + Tr("chat.call_bar.people", {{"count", std::to_string(bar.joined)}});
  }
  return text;
}

void CallController::OpenGroupCallPicker(const std::string& thread_id, const bool video_allowed) {
  if (people_picker_notify_.open_for_group_call) {
    people_picker_notify_.open_for_group_call(thread_id, video_allowed);
  }
}

void CallController::OpenMidCallInvitePicker() {
  presenter_.OpenAddPeople();
}

void CallController::OpenMakeGroup() {
  presenter_.OpenMakeGroup();
}

void CallController::JoinGroupInvite() {
  presenter_.JoinGroupInvite();
}

void CallController::PutOffGroupInvite() {
  presenter_.PutOffGroupInvite();
}

void CallController::FillGuestActions(CallInProgressState& in_call, const CallScreen& screen) {
  in_call.guest_actions.clear();
  const std::string local = call_ports_.local_relay_identity ? call_ports_.local_relay_identity().value_or("") : "";
  for (const CallScreen::Member& member : screen.members) {
    if (!member.guest || (!member.can_remove && !member.can_add_to_group)) {
      continue;
    }
    std::string name = DisplayNameForIdentity(member.identity);
    if (name.empty()) {
      name = member.identity;
    }
    CallGuestActionState row;
    row.identity = member.identity.c_str();
    if (!local.empty() && member.brought_by == local) {
      row.line = Tr("call.guest.line_yours", {{"name", name}}).c_str();
    } else {
      std::string bringer = DisplayNameForIdentity(member.brought_by);
      row.line =
          Tr("call.guest.line", {{"name", name}, {"bringer", bringer.empty() ? member.brought_by : bringer}}).c_str();
    }
    if (member.can_add_to_group) {
      row.add_to_group_label = Tr("call.guest.add_to_group", {{"name", name}}).c_str();
    }
    row.can_remove = member.can_remove;
    in_call.guest_actions.push_back(std::move(row));
  }
}

void CallController::RemoveGuestFromCall(const std::string& identity) {
  std::string name = DisplayNameForIdentity(identity);
  if (name.empty()) {
    name = identity;
  }
  UserFeedback::Confirm(
      Tr("call.guest.remove_title", {{"name", name}}), Tr("call.guest.remove_message"),
      [this, identity](const bool ok) {
        if (ok) {
          presenter_.RemoveFromCall(identity);
        }
      },
      Tr("call.guest.remove_ok"));
}

void CallController::AddGuestToGroup(const std::string& identity) {
  std::string name = DisplayNameForIdentity(identity);
  if (name.empty()) {
    name = identity;
  }
  presenter_.AddGuestToGroup(identity, [name](Roe<void> invited) {
    if (invited) {
      UserFeedback::Ok(Tr("call.guest.invited_to_group", {{"name", name}}));
    }
  });
}

void CallController::InviteIdentitiesToActiveCall(const std::vector<std::string>& invitee_identities) {
  presenter_.InviteToActiveCall(invitee_identities);
}

bool CallController::VideoCallAvailable() {
  return presenter_.VideoCallAvailable();
}

void CallController::AcceptIncoming() {
  EndDemoScene();
  (void)presenter_.Accept(false);
}

void CallController::AcceptIncomingVoiceOnly() {
  EndDemoScene();
  (void)presenter_.Accept(true);
}

void CallController::AcceptIncomingWithCharge() {
  (void)presenter_.AcceptWithCharge();
}

void CallController::DeclineIncoming() {
  EndDemoScene();
  (void)presenter_.Decline();
}

// A call that was connected and then lost its path for good says "disconnected" for a moment
// and closes by itself; one that never connected stays open so the user can retry (B89).
void CallController::EndDisconnectedCall(const int64_t now_ms) {
  const CallScreen& screen = presenter_.Screen();
  if (screen.kind != CallScreen::Kind::InCall || screen.status != CallScreen::Status::Disconnected) {
    disconnected_since_ms_ = 0;
    return;
  }
  if (disconnected_since_ms_ == 0) {
    disconnected_since_ms_ = now_ms;
    return;
  }
  if (now_ms - disconnected_since_ms_ >= kDisconnectedCloseMs) {
    log().info << "call closed by itself after a failed reconnect call_id=" << screen.call_id;
    disconnected_since_ms_ = 0;
    LeaveActive();
  }
}

void CallController::LeaveActive() {
  EndDemoScene();
  (void)presenter_.Leave();
  StopRingback(Backend());
}

void CallController::RetryConnect() {
  presenter_.Retry();
}

// On hold (another call has the audio) PP muted the mic and paused the camera itself; a tap on those buttons meanwhile
// hands the choice to the user (the end of the hold then changes nothing) — never a locked button (Kenneth 2026-11-06:
// if the OS never says the hold ended, the user must be able to unmute by hand).
void CallController::ToggleMute() {
  if (local_on_hold_) {
    hold_mute_.UserToggled();
  }
  presenter_.ToggleMute();
}

void CallController::ToggleCamera() {
  if (local_on_hold_ && video_paused_) {
    // The hold only paused the camera (it still counts as on): a tap brings it back now, and the user keeps it from here.
    video_paused_ = false;
    if (auto* backend = Backend(); backend && backend->Available()) {
      backend->SetLocalVideoPaused(false);
    }
    return;
  }
  presenter_.ToggleCamera();
}

void CallController::FlipCamera() {
  presenter_.FlipCamera();
}

void CallController::ToggleSpeaker() {
  presenter_.ToggleSpeaker();
}

void CallController::ApplyAudioLevels(IMediaView& media) {
  media.RefreshRemoteVideoHealth();
  BindVideoFrameWake(media);

  auto& in_call = in_call_;
  const bool muted = media.IsMuted();
  in_call.muted = muted;
  if (auto failure = media.TakeCameraFailure()) {
    // Camera opens asynchronously (media device thread): report the miss. A camera that only waits
    // for the device (busy, or not responding yet) stays on and comes on by itself (L016); any
    // other miss withdraws the video flag the roster already advertised.
    presenter_.ReportCameraFailure(*failure);
    if (auto* failed_backend = Backend();
        !media.CameraWaitingForDevice() && failed_backend && failed_backend->Available()) {
      failed_backend->SetLocalVideoEnabled(false);
    }
  }
  in_call.camera_on = media.IsCameraEnabled();
  // The facing actually open (it changes when the other camera has opened, not on the tap): the flip button is lit
  // while the front camera is in use (#83).
  in_call.camera_front = in_call.camera_on && media.CameraFrontFacing();
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
  in_call.show_speaker = AudioSession::SupportsSpeakerToggle();
  in_call.speaker_on = AudioSession::IsSpeakerphoneOn();
  auto* backend = Backend();
  in_call.show_camera = false;
  if (backend && backend->Available() && !presenter_.ActiveCallId().empty()) {
    if (auto allowed = backend->VideoAllowedForCall(presenter_.ActiveCallId());
        allowed && allowed->has_value() && **allowed) {
      in_call.show_camera = ShowsCameraButton(
          media.VideoEncoderAvailable(),
          CallMediaAdaptation::CameraPathAllowsVideo(media.PathPressure(), media.UplinkBudget()), in_call.camera_on);
      camera_sync_off_done_ = false;
    } else if (!camera_sync_off_done_ && media.IsCameraEnabled() && allowed && allowed->has_value() &&
               !**allowed) {
      // M4: only narrow (turn camera off) when VideoAllowedForCall explicitly returned false — a
      // read error or unknown value keeps show_camera hidden above but must not touch the camera.
      // V035: a voice-only Accept (ours or the peer's) narrowed video_allowed to false after the
      // local camera was already on (e.g. caller turned it on before the callee answered voice-only)
      // — turn it off through the normal path, once.
      camera_sync_off_done_ = true;
      log().info << "ApplyAudioLevels camera off (video_allowed=false) call_id=" << presenter_.ActiveCallId();
      backend->SetLocalVideoEnabled(false, [](Roe<void> cam) {
        if (!cam) {
          UserFeedback::Fail(cam.error().message);
        }
      });
    }
  }
  // Whatever VideoAllowedForCall said (false, error), a sending camera can always be turned off.
  if (!in_call.show_camera) {
    in_call.show_camera = ShowsCameraButton(media.VideoEncoderAvailable(), /*path_allows_video=*/false, in_call.camera_on);
  }
  in_call.show_flip = ShowsCameraFlip(Platform::IsMobile(), in_call.show_camera, in_call.camera_on);

  bool peer_camera_on = false;
  bool have_peer_video_flag = false;
  peer_on_hold_ = false;
  peer_video_paused_ = false;
  if (backend && backend->Available() && !presenter_.ActiveCallId().empty()) {
    if (auto peer_media = backend->PeerMediaForCall(presenter_.ActiveCallId()); peer_media && peer_media->has_value()) {
      peer_camera_on = (*peer_media)->video_enabled;
      peer_on_hold_ = (*peer_media)->on_hold;
      peer_video_paused_ = (*peer_media)->video_paused;
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
    log().info << "peer video call_id=" << presenter_.ActiveCallId() << " roster=" << roster_state
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
  if (backend && backend->Available() && !presenter_.ActiveCallId().empty() && presenter_.VideoIntentCallId() == presenter_.ActiveCallId() &&
      !media.EverHadRemoteVideo() && backend->Phase() != CallPhase::OutboundCalling) {
    if (auto allowed = backend->VideoAllowedForCall(presenter_.ActiveCallId()); allowed && allowed->has_value() && **allowed) {
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
  const bool expect_remote_video = !have_peer_video_flag || peer_camera_on;
  const bool missing_after_video =
      media.EverHadRemoteVideo() && expect_remote_video && !media.HasRemoteVideo();
  // A call that was live and then failed reads "disconnected" (CallScreen decides); the mesh
  // "couldn't connect" copy below is for calls that never connected.
  const bool mesh_messaging_failed = backend && backend->Available() && backend->IsDirectConnectFailed() &&
                                     !backend->IsAwaitingSfuRecovery() && backend->MediaPath() != CallMediaPath::Hop &&
                                     presenter_.Screen().status != CallScreen::Status::Disconnected;
  const bool path_lost =
      backend && backend->Available() && backend->MediaStatus() == CallMediaStatus::Reconnecting;
  // On hold (here or there) the audio is silence by design: only a lost path is a reconnect.
  const bool on_hold = local_on_hold_ || peer_on_hold_;
  in_call.on_hold = on_hold;
  const bool media_reconnect =
      !mesh_messaging_failed &&
      (path_lost || (!on_hold && (stalling || missing_after_video ||
                                  (backend && backend->Available() &&
                                   CallMediaDropped(*backend, media, presenter_.ActiveCallId())))));
  reconnecting_now_ = media_reconnect;

  // Live or soft-stall: keep painting the last frame. Hard stall / camera-off clears HasRemoteVideo.
  in_call.remote_video = media.HasRemoteVideo() && expect_remote_video;
  if (!in_call.remote_video) {
    CallVideoTileRenderer::Instance().ClearRemote();
  }

  IMediaView::VideoTileFrame local_tile;
  in_call.local_preview = media.CopyLocalVideoFrame(local_tile);
  bool self_new = false;
  bool peer_new = false;
  if (in_call.local_preview) {
    self_new = local_tile.seq != last_shown_self_seq_;
    last_shown_self_seq_ = local_tile.seq;
    CallVideoTileRenderer::Frame frame;
    frame.width = local_tile.width;
    frame.height = local_tile.height;
    frame.seq = local_tile.seq;
    frame.rgba = std::move(local_tile.rgba);
    CallVideoTileRenderer::Instance().SubmitLocalFrame(std::move(frame));
  }
  if (in_call.remote_video) {
    IMediaView::VideoTileFrame remote_tile;
    if (media.CopyRemoteVideoFrame(remote_tile)) {
      peer_new = remote_tile.seq != last_shown_peer_seq_;
      last_shown_peer_seq_ = remote_tile.seq;
      CallVideoTileRenderer::Frame frame;
      frame.width = remote_tile.width;
      frame.height = remote_tile.height;
      frame.seq = remote_tile.seq;
      frame.rgba = std::move(remote_tile.rgba);
      CallVideoTileRenderer::Instance().SubmitRemoteFrame(std::move(frame));
    }
  }

  const int64_t now_ms = util::NowUnixMs();
  metrics_.NoteVideoFrame(in_call.local_preview, self_new, in_call.remote_video, peer_new, now_ms);
  std::vector<uint32_t> live_peer_streams;
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
    IMediaView::VideoTileFrame peer_tile;
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
  ApplySpotlightToState(in_call); // the spotlight / strip copies take the rows' new video flags

  if (backend && backend->Available() && !presenter_.ActiveCallId().empty()) {
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
        backend->RequestVideoRefresh(presenter_.ActiveCallId(), row.identity.c_str());
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
          backend->RequestVideoRefresh(presenter_.ActiveCallId(), row.identity.c_str());
        }
      } else if (auto peer = backend->PeerIdentityForCall(presenter_.ActiveCallId()); peer && peer->has_value()) {
        backend->RequestVideoRefresh(presenter_.ActiveCallId(), **peer);
      }
    }
  }

  // A reconnect keeps the stage only when video was part of the call; a voice call's reconnect
  // must not open an empty "camera off" stage (Kenneth 2026-10-23).
  const bool video_in_play = in_call.camera_on || media.EverHadRemoteVideo() || (have_peer_video_flag && peer_camera_on);
  in_call.stage_visible = in_call.camera_on || in_call.remote_video || in_call.local_preview ||
                          (have_peer_video_flag && peer_camera_on) || (media_reconnect && video_in_play) ||
                          starting_video || peer_video_paused_;
  if (in_call.remote_video) {
    in_call.remote_placeholder = "";
  } else if (starting_video) {
    in_call.remote_placeholder = Tr("call.placeholder.starting_video").c_str();
  } else if (peer_on_hold_) {
    in_call.remote_placeholder = Tr("call.placeholder.on_hold").c_str();
  } else if (have_peer_video_flag && !peer_camera_on) {
    in_call.remote_placeholder =
        Tr(peer_video_paused_ ? "call.placeholder.camera_paused" : "call.placeholder.camera_off").c_str();
  } else if (mesh_messaging_failed) {
    in_call.remote_placeholder = Tr("call.status.couldnt_connect").c_str();
  } else {
    in_call.remote_placeholder = "";
  }

  // The call timer counts the call (first live), not the current path: it runs on through a reconnect.
  const int64_t call_live_since_ms = backend ? backend->LiveSinceMs(presenter_.ActiveCallId()) : 0;
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
  } else if (path_lost ||
             (media_reconnect && backend && CallMediaDropped(*backend, media, presenter_.ActiveCallId()))) {
    // The call lost its path, or its media stopped being live while it runs.
    in_call.elapsed = FormatElapsed(call_live_since_ms);
    in_call.subtitle = in_call.elapsed;  // greyed; the capsule says "Reconnecting…"
  } else if (stalling) {
    // Remote video stalled: the timer keeps running (greyed); the capsule says "Reconnecting…".
    in_call.elapsed = FormatElapsed(call_live_since_ms);
    in_call.subtitle = in_call.elapsed;
  } else if (backend && backend->Available() && backend->MediaChromeLive()) {
    // Provisional Connected — ApplyMediaHealth may override for NoAudio / TX-only / Degraded.
    in_call.elapsed = FormatElapsed(call_live_since_ms);
    if (!in_call.elapsed.empty()) {
      in_call.subtitle = in_call.elapsed;
    } else {
      in_call.subtitle = Tr("call.status.connected").c_str();
    }
  }

  ApplyMediaHealth(media, backend, media_reconnect || mesh_messaging_failed);
  if (on_hold && !mesh_messaging_failed && !path_lost) {
    // Silence by design, not a broken path: no "no audio" hint, and the line says why.
    in_call.quality_hint = "";
    if (local_on_hold_) {
      in_call.subtitle = Tr("call.status.on_hold_local").c_str();
    } else if (const std::string name = DisplayNameForIdentity(presenter_.Screen().peer_identity); !name.empty()) {
      in_call.subtitle = Tr("call.status.on_hold_peer_named", {{"name", name}}).c_str();
    } else {
      in_call.subtitle = Tr("call.status.on_hold_peer").c_str();
    }
  }
}

CallMediaHealthView CallController::BuildMediaHealthView(IMediaView& media, CallUiBackend* backend,
                                                         const bool media_reconnect) const {
  CallMediaHealthInput in;
  in.engine = media.HealthSnapshot();
  if (backend && backend->Available()) {
    in.hop = backend->HopHealth();
    in.reach_path_kind = backend->MediaPathKind();
  }
  in.now_ms = util::NowUnixMs();
  in.reconnecting = media_reconnect;
  if (link_sample_call_id_ == presenter_.ActiveCallId()) {
    in.link = link_health_;
  }
  return EvaluateCallMediaHealth(in);
}

void CallController::SampleLinkHealth(CallUiBackend* backend, const int64_t now_ms) {
  if (link_sample_call_id_ != presenter_.ActiveCallId()) {
    link_sample_call_id_ = presenter_.ActiveCallId();
    link_counters_ = {};
    link_health_ = {};
    last_link_sample_ms_ = 0;
  }
  if (last_link_sample_ms_ != 0 && now_ms - last_link_sample_ms_ < 2000) {
    return;
  }
  last_link_sample_ms_ = now_ms;
  const CallLinkCounters now =
      backend && backend->Available() ? backend->MediaLinkCounters() : CallLinkCounters{};
  const CallLinkHealth next = CallLinkHealthBetween(link_counters_, now);
  // Keep the last resend figure while a quiet interval (no Reliable sends) has none of its own.
  const double resend_pct = next.resend_pct >= 0.0 ? next.resend_pct : link_health_.resend_pct;
  link_health_ = next;
  link_health_.resend_pct = next.available ? resend_pct : -1.0;
  link_counters_ = now;
}

void CallController::ApplyMediaHealth(IMediaView& media, CallUiBackend* backend, const bool media_reconnect) {
  auto& in_call = in_call_;
  if (!in_call.active) {
    return;
  }

  SampleLinkHealth(backend, util::NowUnixMs());
  const CallMediaHealthView view = BuildMediaHealthView(media, backend, media_reconnect);
  const int64_t now_ms = util::NowUnixMs();
  NoteActiveSpeaker(view.engine, in_call.muted ? 0.f : media.LocalInputLevel(), now_ms);
  media_hint_.Update(presenter_.ActiveCallId(), view.asymmetry == CallAudioAsymmetry::SendingOnly, now_ms);

  if (metrics_.Tracking() && now_ms - last_metrics_media_ms_ >= 500) {
    last_metrics_media_ms_ = now_ms;
    CallMetricsTracker::MediaTick tick;
    tick.connected = backend && backend->Available() && backend->MediaChromeLive();
    tick.path = view.path_kind;
    tick.quality = view.quality;
    tick.health = media.HealthSnapshot();
    EmitMetrics(metrics_.NoteMedia(presenter_.ActiveCallId(), tick, now_ms, Vitals(now_ms)));
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
  if (const char* hint_key = media_hint_.KeyFor(view.asymmetry, in_call.video); hint_key && hint_key[0]) {
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
    if (const char* hint_key = media_hint_.KeyFor(view.asymmetry, in_call.video); hint_key && hint_key[0]) {
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
  // Phones say how the call goes in plain words (the connection story below); the path word stays on desktop.
  const bool show_path =
      !phone_layout_ && path_key && backend && backend->Available() &&
      backend->MediaStatus() != CallMediaStatus::Reconnecting &&
      (backend->MediaChromeLive() ||
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
    const auto now = std::chrono::steady_clock::now();
    if (in_call.debug_subtitle.empty() || now - last_debug_subtitle_ >= kDebugSubtitleInterval) {
      in_call.debug_subtitle = FormatCallDebugSubtitle(view, now_ms).c_str();
      last_debug_subtitle_ = now;
    }
  } else {
    in_call.debug_subtitle = "";
  }

  if (now_ms - last_media_health_log_ms_ >= 2000) {
    last_media_health_log_ms_ = now_ms;
    log().info << FormatMediaHealthLogLine(view, now_ms, presenter_.ActiveCallId());
  }

  // Toast only real, lasting audio faults (Kenneth 2026-09-28: the old toasts fired on every dip —
  // "Poor" on a jitter spike, "can't hear the other side" in the second before the first audio
  // arrived). Network quality stays visible in the signal bars; an audio fault must persist for
  // CallAudioFaultToastGate::kAudioFaultToastMs and toasts at most once per call.
  const bool audio_fault =
      view.quality == CallPathQuality::NoAudio || view.asymmetry != CallAudioAsymmetry::None;
  if (audio_fault_gate_.Update(presenter_.ActiveCallId(), audio_fault, now_ms)) {
    if (const char* hint_key = media_hint_.KeyFor(view.asymmetry, in_call.video); hint_key && hint_key[0]) {
      UserFeedback::Fail(Tr(hint_key));
    } else if (const char* label_key = CallPathQualityLabelKey(view.quality); label_key && label_key[0]) {
      UserFeedback::Fail(Tr(label_key));
    }
  }
  ApplyConnectStory(presenter_.Screen(), view.path_kind, view.quality, view.quality_bars, now_ms);
}

void CallController::ShowCallDetails() {
  if (!AppRuntime::CurrentlyOnUI()) {
    AppRuntime::PostUI([this]() { ShowCallDetails(); });
    return;
  }
  if (phone_layout_ && in_call_.active) {
    // Phones: the signal bar opens (and its sheet's X, scrim and drag close) "how this call got through".
    story_sheet_open_ = !story_sheet_open_;
    in_call_.story_sheet_open = story_sheet_open_ && !in_call_.phone_failed;
    FillStoryLines();
    SyncShellState();
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
  const bool media_reconnect = CallMediaDropped(*backend, media, presenter_.ActiveCallId());
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
  if (const char* hint_key = media_hint_.KeyFor(view.asymmetry, in_call.video); hint_key && hint_key[0]) {
    copy.asymmetry_hint = Tr(hint_key);
  }
  if (view.link.available && view.link.rtt_ms >= 0) {
    copy.network_label = Tr("call.details.network.rtt", {{"ms", std::to_string(view.link.rtt_ms)}});
    if (view.link.resend_pct >= 0.0) {
      char pct[16];
      std::snprintf(pct, sizeof(pct), "%.1f", view.link.resend_pct);
      copy.network_label += " · " + Tr("call.details.network.resend", {{"pct", pct}});
    }
  }
  copy.call_id = presenter_.ActiveCallId();
  copy.duration_heading = Tr("call.details.duration");
  copy.path_heading = Tr("call.details.path");
  copy.quality_heading = Tr("call.details.quality");
  copy.mic_heading = Tr("call.details.mic");
  copy.incoming_heading = Tr("call.details.incoming");
  copy.note_heading = Tr("call.details.note");
  copy.network_heading = Tr("call.details.network");
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

bool CallController::RenderDemoScene() {
  static const std::pair<std::string, int64_t> spec = []() {
    const char* env = std::getenv("PP_DEMO_CALL");
    return env && env[0] ? ParseDemoCallScene(env) : std::pair<std::string, int64_t>{};
  }();
  if (spec.first.empty()) {
    return false;
  }
  const int64_t now = util::NowUnixMs();
  if (demo_started_ms_ == 0) {
    demo_started_ms_ = now - spec.second;
  }
  if (demo_ended_) {
    return true;
  }
  const DemoCallFrame f = DemoCallSceneAt(spec.first, now - demo_started_ms_, now);
  if (!f.valid) {
    return false;
  }
  phone_layout_ = shell_call_chrome_.phone_layout && shell_call_chrome_.phone_layout();
  if (f.screen.kind == CallScreen::Kind::Ringing) {
    RenderRing(f.screen);
    ringtone_.Stop();
    return true;
  }
  if (f.screen.kind == CallScreen::Kind::Answering) {
    RenderAnswering(f.screen);
    ringtone_.Stop();
    return true;
  }
  ClearRing();
  FillInCallFromScreen(f.screen);
  in_call_.stage_visible = f.video_stage;
  in_call_.show_camera = true;
  in_call_.camera_on = f.video_stage;
  in_call_.camera_front = f.video_stage;
  in_call_.show_flip = f.video_stage;
  in_call_.remote_placeholder = "";
  // V060 (the hold scenes): the other side has the 1:1 call on hold.
  bool peer_on_hold = false;
  for (const CallScreen::Member& member : f.screen.members) {
    peer_on_hold = peer_on_hold || (!f.screen.group && !member.is_local && member.on_hold);
  }
  in_call_.on_hold = peer_on_hold;
  ApplyConnectStory(f.screen, f.path_kind, f.quality, f.quality_bars, now);
  if (peer_on_hold) {
    in_call_.subtitle =
        Tr("call.status.on_hold_peer_named", {{"name", DisplayNameForIdentity(f.screen.peer_identity)}}).c_str();
    in_call_.remote_placeholder = Tr("call.placeholder.on_hold").c_str();
  }
  MediaEngineHealth engine;  // the scene's made-up levels, for the speaking rings and the spotlight
  for (const auto& [identity, level] : f.levels) {
    for (const CallRosterParticipantState& row : in_call_.roster) {
      if (identity == row.identity.c_str()) {
        const auto stream = static_cast<uint32_t>(std::strtoul(row.stream_id.c_str(), nullptr, 10));
        engine.streams.push_back({.stream_id = stream, .peak_level = level});
      }
    }
  }
  NoteActiveSpeaker(engine, f.self_level, now);
  if (f.video_stage) {
    // Stand-in cameras, ours blue and theirs warm, so the 1:1 swap shows on the simulator.
    in_call_.local_preview = true;
    in_call_.remote_video = f.screen.status == CallScreen::Status::Connected && !peer_on_hold;
    if (now - demo_frames_ms_ >= 1000) {
      const uint64_t seq = static_cast<uint64_t>(now);
      static constexpr uint8_t kSelfTop[3] = {40, 90, 200}, kSelfBottom[3] = {20, 30, 70};
      static constexpr uint8_t kPeerTop[3] = {230, 140, 60}, kPeerBottom[3] = {120, 40, 90};
      CallVideoTileRenderer::Instance().SubmitLocalFrame(DemoCameraFrame(seq, kSelfTop, kSelfBottom));
      if (in_call_.remote_video) {
        CallVideoTileRenderer::Instance().SubmitRemoteFrame(DemoCameraFrame(seq, kPeerTop, kPeerBottom));
      } else {
        CallVideoTileRenderer::Instance().ClearRemote();
      }
      demo_frames_ms_ = now;
    }
  }
  SyncShellState();
  return true;
}

void CallController::EndDemoScene() {
  if constexpr (kUiDemoBuild) {
    if (demo_started_ms_ != 0 && !demo_ended_) {
      demo_ended_ = true;
      ClearInCall();
      ClearRing();
      SyncShellState();
    }
  }
}

CallConflictNames CallController::ConflictNames(const std::string& caller_label, const bool conflict,
                                                const CallScreen::EndingCall& ending) {
  CallConflictNames names;
  names.caller = caller_label;
  if (!conflict) {
    return names;
  }
  names.same_peer = ending.same_peer;
  names.ending_group = ending.group;
  if (!ending.group) {
    names.ending = DisplayNameForIdentity(ending.peer_identity);
    return names;
  }
  // A group's call: the group's name, read once per ring (the ring's own group title is GroupTitle's).
  if (ending_title_call_ != ending.call_id) {
    ending_title_call_ = ending.call_id;
    ending_title_.clear();
    if (!ending.thread_id.empty() && call_ports_.get_thread) {
      if (auto thread = call_ports_.get_thread(ending.thread_id); thread && *thread) {
        ending_title_ = (*thread)->local_title.empty() ? (*thread)->title : (*thread)->local_title;
      }
    }
  }
  names.ending = ending_title_;
  return names;
}

std::string CallController::GroupTitle(const std::string& thread_id) {
  // Read each time (a rename shows at once; a thread not readable yet is tried again), at most every 2 s per thread.
  const int64_t now = util::NowUnixMs();
  if (thread_id == group_title_thread_ && !group_title_.empty() && now - group_title_read_ms_ < 2000) {
    return group_title_;
  }
  group_title_thread_ = thread_id;
  group_title_read_ms_ = now;
  group_title_.clear();
  if (!thread_id.empty() && call_ports_.get_thread) {
    if (auto thread = call_ports_.get_thread(thread_id); thread && *thread) {
      group_title_ = (*thread)->local_title.empty() ? (*thread)->title : (*thread)->local_title;
    }
  }
  return group_title_;
}

void CallController::FillRingPhoneFields(const CallScreen& screen, const std::string& caller_label) {
  ring_.group = screen.ring_group;
  if (screen.ring_group) {
    std::string title = GroupTitle(screen.thread_id);
    ring_.phone_title = (title.empty() ? caller_label : title).c_str();
    ring_.phone_subtitle =
        Tr(screen.ring_video ? "call.ring.phone.group_video" : "call.ring.phone.group_voice", {{"name", caller_label}})
            .c_str();
    ring_.has_avatar = false;
    ring_.avatar_src = "";
    ring_.avatar_letter = "";
    ring_.avatar_tone = 2;
    return;
  }
  ring_.phone_title = caller_label.c_str();
  ring_.phone_subtitle = Tr(screen.ring_video ? "call.ring.phone.video" : "call.ring.phone.voice").c_str();
  const AvatarGlyph glyph = MakeAvatarGlyph(caller_label, screen.caller_identity);
  ring_.avatar_letter = glyph.letter.c_str();
  ring_.avatar_tone = glyph.tone;
  const std::string photo =
      call_ports_.identity_icon_local_path ? call_ports_.identity_icon_local_path(screen.caller_identity) : std::string();
  ring_.has_avatar = !photo.empty();
  ring_.avatar_src = photo.c_str();
}

void CallController::FillPhoneCallFields(const CallScreen& screen) {
  auto& in_call = in_call_;
  const bool answering = screen.kind == CallScreen::Kind::Answering;
  const bool group = answering ? screen.ring_group : screen.group;
  const std::string& peer = answering ? screen.caller_identity : screen.peer_identity;
  in_call.video = answering ? screen.answered_video : screen.video;
  const std::string name = peer.empty() ? std::string() : DisplayNameForIdentity(peer);
  if (group) {
    const std::string title = GroupTitle(screen.thread_id);
    in_call.phone_title = title.empty() ? in_call.title : title.c_str();
  } else {
    in_call.phone_title = name.empty() ? Tr("call.label.them").c_str() : name.c_str();
  }
  const auto photo_for = [this](const std::string& identity) {
    return call_ports_.identity_icon_local_path && !identity.empty() ? call_ports_.identity_icon_local_path(identity)
                                                                     : std::string();
  };
  const AvatarGlyph peer_glyph = MakeAvatarGlyph(name, peer);
  in_call.peer_avatar_letter = peer_glyph.letter.c_str();
  in_call.peer_avatar_tone = peer_glyph.tone;
  const std::string peer_photo = photo_for(peer);
  in_call.peer_has_avatar = !peer_photo.empty();
  in_call.peer_avatar_src = peer_photo.c_str();
  std::string self;
  for (const CallScreen::Member& member : screen.members) {
    if (member.is_local) {
      self = member.identity;
    }
  }
  if (self.empty() && call_ports_.local_relay_identity) {
    self = call_ports_.local_relay_identity().value_or(std::string{});
  }
  const AvatarGlyph self_glyph = MakeAvatarGlyph(Tr("call.label.you"), self);
  in_call.self_avatar_letter = self_glyph.letter.c_str();
  in_call.self_avatar_tone = self_glyph.tone;
  const std::string self_photo = photo_for(self);
  in_call.self_has_avatar = !self_photo.empty();
  in_call.self_avatar_src = self_photo.c_str();

  // Couldn't connect / disconnected: the failure page says it plainly; why is in the log (#31 / #37).
  using Status = CallScreen::Status;
  in_call.phone_failed = !answering && (screen.status == Status::CouldntConnect || screen.status == Status::Disconnected);
  if (screen.status == Status::Disconnected) {
    in_call.failed_text = Tr("call.status.disconnected").c_str();
  } else if (group || screen.hint_missing_mic) {
    in_call.failed_text = Tr("call.status.couldnt_connect").c_str();
  } else {
    in_call.failed_text = Tr("call.error.peer_unreachable").c_str();
  }
  in_call.story_sheet_open = story_sheet_open_ && !in_call.phone_failed;
}

void CallController::ApplyConnectStory(const CallScreen& screen, const std::string& path_kind,
                                       const CallPathQuality quality, const int quality_bars, const int64_t now_ms) {
  auto& in_call = in_call_;
  const bool answering = screen.kind == CallScreen::Kind::Answering;
  CallStoryInput in;
  in.call_id = answering ? screen.ring_call_id : screen.call_id;
  in.kind = screen.kind;
  in.status = answering ? CallScreen::Status::Connecting : screen.status;
  in.group = answering ? screen.ring_group : screen.group;
  in.placed_here = !answering && screen.placed_here;
  in.media_connected = !answering && screen.media_connected;
  in.path_kind = path_kind;
  in.quality = quality;
  in.quality_bars = quality_bars;
  in.now_ms = now_ms;
  const CallStoryView& v = story_.Step(in);
  if (in.call_id != story_logged_call_) {
    story_logged_call_ = in.call_id;
    story_events_logged_ = 0;
  }
  if (v.events.size() != story_events_logged_) {
    // The story's moments with the technical words the screen never shows (TODO #76: the detail goes to the log).
    for (size_t i = story_events_logged_; i < v.events.size(); ++i) {
      log().info << "call story call_id=" << in.call_id << " event=" << static_cast<int>(v.events[i].kind)
                 << " at_ms=" << v.events[i].at_ms << " path=" << path_kind;
    }
    story_events_logged_ = v.events.size();
  }
  if (!phone_layout_) {
    return;  // desktop keeps its bar as it was
  }
  const std::string name = in_call.phone_title.c_str();
  in_call.story_active = v.connecting && !in_call.phone_failed;
  in_call.story_stage = v.stage;
  in_call.story_step_count = 3;
  in_call.story_step4 = "";
  in_call.story_step5 = "";
  if (in.group) {
    in_call.story_step1 = Tr(v.stage == 0 ? "call.story.step.group_calling" : "call.story.step.group_joined").c_str();
    in_call.story_step2 = Tr("call.story.step.group_line").c_str();
    in_call.story_step3 = Tr("call.story.step.group_heard").c_str();
  } else {
    // The five steps (#172): calling, answered, matching the fastest line, setting up the encrypted channel,
    // connected. A device that answered starts at "Answered": four lines. The presenter's stage numbers the five.
    const bool matched = v.stage > CallConnectStory::kStageMatching;
    const char* matching_key = "call.story.step.matching";
    if (matched) {
      matching_key = v.route == CallRoute::Relayed ? "call.story.step.matched_relayed"
                     : (v.route == CallRoute::Unknown ? "call.story.step.matched" : "call.story.step.matched_direct");
    }
    std::vector<std::string> lines;
    if (v.outgoing) {
      lines.push_back(Tr("call.story.step.calling", {{"name", name}}));
      lines.push_back(Tr("call.story.step.peer_answered", {{"name", name}}));
    } else {
      lines.push_back(Tr("call.story.step.you_answered"));
    }
    lines.push_back(Tr(matching_key));
    lines.push_back(Tr("call.story.step.securing"));
    lines.push_back(Tr("call.story.step.connected"));
    in_call.story_step_count = static_cast<int>(lines.size());
    // The presenter numbers the five steps; a device that answered has no "calling" line, so it counts one less.
    const int first = v.outgoing ? 0 : 1;
    in_call.story_stage =
        v.stage == CallConnectStory::kStageDone ? in_call.story_step_count : v.stage - first;
    in_call.story_step1 = lines[0].c_str();
    in_call.story_step2 = lines[1].c_str();
    in_call.story_step3 = lines[2].c_str();
    if (lines.size() > 3) {
      in_call.story_step4 = lines[3].c_str();
    }
    if (lines.size() > 4) {
      in_call.story_step5 = lines[4].c_str();
    }
  }
  in_call.result_text =
      v.result_shown ? Tr("call.story.result", {{"how", Tr(CallRouteResultKey(v.result_route))},
                                                {"seconds", FormatTenths(v.connect_tenths)}})
                           .c_str()
                     : "";
  in_call.result_fading = v.result_shown && v.result_fading;
  in_call.signal_shown = v.signal_shown && !in_call.phone_failed;
  in_call.signal_bars = v.signal_bars;
  in_call.signal_tone = v.signal_tone == CallSignalTone::Good ? 0 : (v.signal_tone == CallSignalTone::Fair ? 1 : 2);
  const char* word = CallSignalWordKey(v.signal_quality);
  in_call.signal_word = word[0] ? Tr(word).c_str() : "";
  const char* notice = CallStoryNoticeKey(v.notice);
  in_call.notice_text = notice[0] ? Tr(notice).c_str() : "";
  in_call.notice_warn = v.notice == CallStoryNotice::Slow;
  const CallReconnectView& rc =
      reconnect_.Step(in.call_id, in.status == CallScreen::Status::Reconnecting || reconnecting_now_, now_ms);
  const char* rc_key = CallReconnectKey(rc.kind);
  in_call.reconnect_text = rc_key[0] ? Tr(rc_key).c_str() : "";
  in_call.reconnect_ok = rc.kind == CallReconnectKind::Restored;
  in_call.reconnect_fading = rc.fading;
  in_call.reconnect_dim = rc.timer_dim;
  in_call.notice_fading = v.notice_fading;
  if (story_sheet_open_) {
    FillStoryLines();
  }
}

void CallController::FillStoryLines() {
  auto& in_call = in_call_;
  const CallStoryView& v = story_.View();
  const std::string name = in_call.phone_title.c_str();
  const bool group = in_call.show_roster;
  in_call.story_lines.clear();
  for (size_t i = 0; i < v.events.size(); ++i) {
    const CallStoryEvent& event = v.events[i];
    CallStoryLine line;
    if (i == 0 || (event.at_ms == 0 && event.call_ms < 0)) {
      // Where the story starts (or a call found already live): the time of day, never a "0.0 s".
      line.time = MessageClockLabel(event.wall_ms);
    } else if (event.call_ms >= 0) {
      line.time = FormatMinSec(event.call_ms / 1000);  // after connect: on the call's clock
    } else {
      line.time = Tr("call.story.seconds", {{"seconds", FormatTenths((event.at_ms + 50) / 100)}});
    }
    using Kind = CallStoryEvent::Kind;
    switch (event.kind) {
    case Kind::Placed:
      line.text = Tr("call.story.event.placed", {{"name", name}});
      break;
    case Kind::Answered:
      line.text = Tr(group ? "call.story.event.joined" : "call.story.event.answered");
      break;
    case Kind::PeerAnswered:
      line.text = Tr("call.story.event.peer_answered", {{"name", name}});
      break;
    case Kind::Connected:
      line.text = Tr(group ? "call.story.event.group_connected" : CallRouteEventKey(event.route));
      line.secure = true;
      break;
    case Kind::Encrypted:
      line.text = Tr(group ? "call.story.event.group_encrypted" : "call.story.event.encrypted");
      line.secure = true;
      break;
    case Kind::Closer:
      line.text = Tr("call.story.event.closer", {{"name", name}});
      line.secure = true;
      break;
    }
    in_call.story_lines.push_back(std::move(line));
  }
  in_call.story_how = Tr(CallRouteHowKey(v.route)).c_str();
}

void CallController::RefreshCallLevels() {
  if (presenter_.ActiveCallId().empty()) {
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
