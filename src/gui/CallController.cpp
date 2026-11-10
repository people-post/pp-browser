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
#include "feature/presenters/CallRingSteps.h"
#include "feature/presenters/CallStepScroll.h"
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
              .display_name = [this](const std::string& identity) { return CallerNameFor(identity).label; },
              .now_ms = {},
          }),
      local_hold_(LocalHoldPorts()) {
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
  callbacks.confirm_end_current_call = [](std::function<void(bool)> answer) {
    // "End and call" in the danger colour: it ends the live call (Kenneth 2026-11-09).
    UserFeedback::ConfirmDestructive(Tr("call.replace.title"), Tr("call.replace.body"), std::move(answer),
                                     Tr("call.replace.action"));
  };
  callbacks.on_hold_here = [this]() { return local_hold_.OnHold(); };
  callbacks.show_current_call = [this]() {
    if (chrome_mode_ == CallChromeMode::Minimized) {
      RestoreChromeFromMinimized();
    }
  };
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

CallLocalHold::Ports CallController::LocalHoldPorts() {
  CallLocalHold::Ports ports;
  ports.mic_muted = [this]() {
    auto* backend = Backend();
    return backend && backend->Available() && backend->Media().IsMuted();
  };
  ports.camera_on = [this]() {
    auto* backend = Backend();
    return backend && backend->Available() && backend->Media().IsCameraEnabled();
  };
  ports.publish_hold = [this](const bool on_hold, std::function<void(Roe<void>)> done) {
    auto* backend = Backend();
    if (!backend || !backend->Available()) {
      done(Error("calls unavailable"));
      return;
    }
    backend->SetLocalOnHold(on_hold, std::move(done));
  };
  ports.set_mic_muted = [this](const bool muted) {
    if (auto* backend = Backend(); backend && backend->Available()) {
      backend->SetLocalAudioMuted(muted);
    }
  };
  ports.set_camera_paused = [this](const bool paused) {
    if (auto* backend = Backend(); backend && backend->Available()) {
      backend->SetLocalVideoPaused(paused);
    }
  };
  ports.note = [](const std::string& key) { UserFeedback::Note(Tr(key)); };
  return ports;
}

void CallController::SyncLocalHold() {
  auto* backend = Backend();
  const bool in_call = backend && backend->Available() && !presenter_.ActiveCallId().empty();
  local_hold_.Sync({.call_id = in_call ? presenter_.ActiveCallId() : std::string(),
                    .audio_taken = in_call && AudioSession::IsInterrupted(),
                    .foreground = AppLifecycle::IsForeground(),
                    .now_ms = util::NowUnixMs()});
}

void CallController::OnAppBackground() {
  auto* backend = Backend();
  if (!Platform::IsMobile() || !backend || !backend->Available() || presenter_.ActiveCallId().empty()) {
    return;
  }
  local_hold_.OnAppBackground();
}

void CallController::OnAppForeground() {
  local_hold_.OnAppForeground();
}

namespace {

/** One key for an avatar written into the call page's markup: a change remounts it. */
std::string AvatarKey(const bool group, const bool has_avatar, const ui::String& src, const ui::String& letter,
                      const int tone) {
  return std::string(group ? "g" : "p") + (has_avatar ? std::string(src.c_str()) : std::string(letter.c_str())) + "|" +
         std::to_string(tone);
}

/** The ring page's three bound lines and our own avatar: a change dirties them. */
std::string RingStepsKey(const CallRingState& ring) {
  return std::string(ring.step1.c_str()) + "|" + ring.step2.c_str() + "|" + ring.step3.c_str() + "|" +
         (ring.step2_current ? "c" : "-") + (ring.step3_done ? "d" : "-") + "|" +
         AvatarKey(false, ring.self_has_avatar, ring.self_avatar_src, ring.self_avatar_letter, ring.self_avatar_tone);
}

/** What the answering page's list writes into its markup besides the four steps: lead rows, spark side, clip. */
std::string StoryLeadKey(const CallInProgressState& in_call) {
  std::string key = in_call.story_inbound ? "in" : "out";
  for (const std::string& line : in_call.story_lead) {
    key += "|" + line;
  }
  return key + "|" + std::to_string(in_call.story_scroll_rows) + "|" + std::to_string(in_call.story_area_dp);
}

std::string RosterKey(const std::vector<CallRosterParticipantState>& roster) {
  std::string key;
  for (const CallRosterParticipantState& p : roster) {
    key += std::string(p.name.c_str()) + "|" + p.stream_id.c_str() + "|" + (p.audio_muted ? "m" : "-") +
           (p.video_enabled ? "v" : "-") + (p.has_remote_video ? "r" : "-") + (p.on_hold ? "h" : "-") +
           (p.video_paused ? "p" : "-") + (p.guest ? "g" : "-") + (p.avatar_unknown ? "u" : "-") +
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
      .ring_awaiting_invite = ring.awaiting_invite,
      .ring_caller_unknown = ring.caller_unknown,
      .ring_steps = RingStepsKey(ring),
      .in_call_phone_title = in_call.phone_title.c_str(),
      .in_call_video = in_call.video,
      .in_call_avatars = AvatarKey(false, in_call.peer_has_avatar, in_call.peer_avatar_src, in_call.peer_avatar_letter,
                                   in_call.peer_avatar_tone) +
                         (in_call.peer_unknown ? "u" : "-") +
                         AvatarKey(false, in_call.self_has_avatar, in_call.self_avatar_src, in_call.self_avatar_letter,
                                   in_call.self_avatar_tone),
      .in_call_phone_failed = in_call.phone_failed,
      .in_call_failed_text = in_call.failed_text.c_str(),
      .in_call_story_active = in_call.story_active,
      .in_call_story_stage = in_call.story_stage,
      .in_call_story_steps = std::string(in_call.story_step1.c_str()) + "|" + in_call.story_step2.c_str() + "|" +
                             in_call.story_step3.c_str() + std::string("|") + in_call.story_step4.c_str() + "|" +
                             in_call.story_step5.c_str() + "|" + std::to_string(in_call.story_step_count) + "|" +
                             StoryLeadKey(in_call),
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

/** An engine frame as the tiles take it. */
CallVideoTileRenderer::Frame TileFrame(IMediaView::VideoTileFrame&& tile) {
  CallVideoTileRenderer::Frame frame;
  frame.width = tile.width;
  frame.height = tile.height;
  frame.seq = tile.seq;
  frame.rgba = std::move(tile.rgba);
  return frame;
}

/** The remote stage's words where there is no picture ("": none). */
const char* PlaceholderKey(const CallStagePlaceholder placeholder) {
  switch (placeholder) {
    case CallStagePlaceholder::StartingVideo:
      return "call.placeholder.starting_video";
    case CallStagePlaceholder::CameraOff:
      return "call.placeholder.camera_off";
    case CallStagePlaceholder::CameraPaused:
      return "call.placeholder.camera_paused";
    case CallStagePlaceholder::None:
      break;
  }
  return "";
}

uint32_t RowStream(const CallRosterParticipantState& row) {
  return static_cast<uint32_t>(std::strtoul(row.stream_id.c_str(), nullptr, 10));
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
    if (RenderDemoScene(util::NowUnixMs())) {
      return; // the demo app's scripted call (PP_DEMO_CALL) stands in for the presenter
    }
  }
  presenter_.Tick();     // expired rings; the in-call screen (Calling… / timer / levels) every frame
  StepWakePlaceholder(); // before the ring's pulse / ringtone below: it may start or end the ring page
  const int64_t now = util::NowUnixMs();
  if (ring_.active && !phone_layout_) { // the phone ring page has no pulse: no dirty, no forced frame
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
    const bool call_live = (ring_.active && !ring_.awaiting_invite) || !presenter_.ActiveCallId().empty() ||
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
      .Add("proc_cpu_ms",
           (vitals.cpu_s >= 0 && prev_proc_cpu_s >= 0) ? (vitals.cpu_s - prev_proc_cpu_s) * 1000.0 : -1.0)
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
  presenter_.NoteMessagingReady(); // the vault is unlocked: the connecting page's no-ring clock starts
}

void CallController::StepWakePlaceholder() {
  // Read here (not the last Render's): before the vault is unlocked nothing has rendered yet.
  phone_layout_ = shell_call_chrome_.phone_layout && shell_call_chrome_.phone_layout();
  const CallPresenter::WakeStep step = presenter_.StepWakePlaceholder(phone_layout_);
  if (step.ring == CallPresenter::WakeRing::Show) {
    if (!ring_.awaiting_invite) {
      RenderAwaitingRing();
    } else {
      // The page is up: once the vault unlocks our own photo can be read; it replaces the letter in place.
      const ui::String shown = ring_.self_avatar_src;
      const bool had = ring_.self_has_avatar;
      FillRingSelfAvatar();
      if (had != ring_.self_has_avatar || shown != ring_.self_avatar_src) {
        SyncShellState();
      }
    }
  } else if (step.ring == CallPresenter::WakeRing::Clear && ring_.awaiting_invite) {
    ClearRing();
    SyncShellState();
  }
  if (step.outcome != CallWakePlaceholder::Outcome::None) {
    EmitWakeRingMetric(step);
  }
}

void CallController::RenderAwaitingRing() {
  const CallScreen no_call; // no conflict, no price, voice: the plain Accept / Decline words
  const CallConflictCopy copy = MakeCallConflictCopy(false, {});
  ring_ = {};
  ring_.active = true;
  ring_.awaiting_invite = true;
  ring_.accept_label = copy.accept_label;
  ring_.decline_label = copy.decline_label;
  FillRingAnswerLabels(no_call);
  ring_.phone_title = Tr("call.ring.awaiting.name").c_str();
  ring_.avatar_letter = ""; // the blank default avatar, no "?"
  FillRingSteps(true, "");
  FillRingSelfAvatar();
  SyncShellState();
  SyncRingtone();
}

void CallController::EmitWakeRingMetric(const CallPresenter::WakeStep& view) {
  using Outcome = CallWakePlaceholder::Outcome;
  MetricsLine line("call.wake_ring");
  switch (view.outcome) {
    case Outcome::Ring:
      line.Add("outcome", "ring");
      break;
    case Outcome::Timeout:
      line.Add("outcome", "timeout");
      break;
    default:
      line.Add("outcome", "expired");
      break;
  }
  line.Add("shown_ms", view.shown_ms);
  if (view.ready_ms) {
    line.Add("ready_ms", *view.ready_ms);
  }
  line.Emit();
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
  story_sheet_open_ = false;
  group_titles_.clear();
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
      restore_mode_ = chrome_mode_ == CallChromeMode::Immersive ? CallChromeMode::Immersive : CallChromeMode::Expanded;
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

void CallController::NoteActiveSpeaker(const MediaEngineHealth& engine, const float self_level, const int64_t now_ms) {
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
    ApplySpotlightToState(in_call_); // the render's SyncShellState pushes it
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
  SetChromeMode(restore_mode_ == CallChromeMode::Immersive ? CallChromeMode::Immersive : CallChromeMode::Expanded);
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
                               backend->Phase() == CallPhase::OutboundCalling && !backend->Media().IsActive();
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
      std::string name = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (!name.empty()) {
        return name;
      }
    }
  }
  return {}; // nothing known: the callers say "Stranger" (CallerNameFor), never the account id
}

CallerName CallController::CallerNameFor(const std::string& identity) const {
  return MakeCallerName(DisplayNameForIdentity(identity));
}

std::string CallController::FormatTimer(const int64_t since_ms, const int64_t now_ms) {
  if (since_ms <= 0) {
    return {};
  }
  return FormatMinSec((now_ms - since_ms) / 1000);
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
// ("call-media: peer not reached [...]") mapped to a plain sentence and logged raw (B89); payment
// errors keep their own mapping. A dropped call never gets here: its call screen says it (K014).
std::string CallController::NoticeUserText(const CallNotice& notice) const {
  if (!notice.key.empty()) {
    return Tr(notice.key);
  }
  if (const char* key = CallMediaErrorUserKey(notice.text)) {
    log().info << "call media error shown as " << key << " raw=" << notice.text;
    return Tr(key);
  }
  if (CallNoticeIsLocalizedCopy(notice.text)) {
    return notice.text;
  }
  return PaymentErrorUserMessage(notice.text);
}

void CallController::ShowNotice(const CallNotice& notice) {
  switch (notice.kind) {
    case CallNotice::Kind::Error:
      UserFeedback::Fail(NoticeUserText(notice));
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
      const std::string name = CallerNameFor(notice.identity).label;
      const std::string local = call_ports_.local_relay_identity ? call_ports_.local_relay_identity().value_or("") : "";
      if (!local.empty() && notice.brought_by == local) {
        UserFeedback::Ok(Tr("call.guest.joined_yours", {{"name", name}}));
        return;
      }
      UserFeedback::Ok(Tr("call.guest.joined", {{"name", name}, {"bringer", CallerNameFor(notice.brought_by).label}}));
      return;
    }
    case CallNotice::Kind::IncomingCall: {
      if (system_call_screen_.TakesRing(AppLifecycle::IsVisible(), notice.conflict)) {
        return; // the phone's own call screen rings it (Android's call notification)
      }
      const std::string caller = DisplayNameForIdentity(notice.identity);
      if (notice.conflict) {
        // V064: over a call, the alert says who is calling and which call answering ends, as the ring does.
        const CallConflictCopy copy =
            MakeCallConflictCopy(true, ConflictNames(caller, true, notice.ending, util::NowUnixMs()));
        ILocalNotifier::Instance().NotifyIncoming(copy.title, copy.hint, "");
        return;
      }
      // No contact name: "Stranger is calling you"; no identity at all: "Someone is calling you".
      const std::string body = !caller.empty()            ? Tr("call.notify.body_named", {{"name", caller}})
                               : !notice.identity.empty() ? Tr("call.notify.body_stranger")
                                                          : Tr("call.notify.body_unknown");
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
          (void)presenter_.CallFromChat(thread_id, video); // as the chat's call button: joins, or voice without video
        }
      },
      Tr("call.call_back.action"));
}

void CallController::Render() {
  phone_layout_ = shell_call_chrome_.phone_layout && shell_call_chrome_.phone_layout();
  const CallScreen& screen = presenter_.Screen();
  const int64_t now_ms = util::NowUnixMs(); // one clock for everything this render words
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
      RenderAnswering(screen, presenter_.MediaScreen(), now_ms);
      return;
    case CallScreen::Kind::Ringing:
      if (system_call_screen_.SystemRings(screen.ring_call_id)) {
        // The phone's own call screen rings it (Android's call notification, P014): one ring, not the app's dialog too.
        ClearRing();
        SyncShellState();
        SyncRingtone();
      } else {
        RenderRing(screen, now_ms);
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
        RenderInCall(screen, presenter_.MediaScreen(), *backend, now_ms);
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
      // "Incoming call, connecting…" has no call screen behind it: only StepWakePlaceholder takes it down.
      if (!ring_.awaiting_invite) {
        ClearRing();
      }
      SyncShellState();
      return;
  }
}

void CallController::RenderAnswering(const CallScreen& screen, const CallMediaScreen& frame, const int64_t now_ms) {
  // Accept in flight (CALLS.md): the ring is gone; a Connecting bar so Accept does not look hung.
  ClearRing();
  in_call_.active = true;
  in_call_.call_id = screen.ring_call_id;
  in_call_.subtitle = Tr("call.status.connecting").c_str();
  in_call_.title = screen.ring_video ? Tr("call.title.video").c_str() : Tr("call.title.voice").c_str();
  in_call_.peer_label = CallerNameFor(screen.caller_identity).label.c_str();
  // Answered with video: switch to the video stage right away (see CallMediaFrame::kVideoStartMs).
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
    ResetChromeForCall(screen.ring_call_id, true);
    ApplyChromeModeToState(in_call_);
    FillPhoneCallFields(screen, now_ms);
    ApplyConnectStory(frame);
  }
  SyncShellState();
  SyncRingtone();
}

void CallController::RenderRing(const CallScreen& screen, const int64_t now_ms) {
  const std::string caller_label = CallerNameFor(screen.caller_identity).label;
  const CallConflictCopy copy =
      MakeCallConflictCopy(screen.conflict, ConflictNames(caller_label, screen.conflict, screen.ending, now_ms));

  // Hide the in-call bar while ringing a *different* call (the call itself stays active).
  HideInCallChrome();

  ring_.active = true;
  ring_.awaiting_invite = false; // the placeholder page, if it was up, is overwritten in place
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
  FillRingAnswerLabels(screen);
  FillRingPhoneFields(screen, caller_label, now_ms);
  SyncShellState();
  SyncRingtone();
}

void CallController::FillRingAnswerLabels(const CallScreen& screen) {
  // V035: video ring offers a voice-answer alternative; the primary button reads
  // "Video answer" so it's clear Accept keeps the camera option (conflict wording unchanged).
  if (ring_.video_allowed && !screen.conflict) {
    ring_.accept_label = Tr("call.ring.accept_video").c_str();
  }
  FillRingPricing(screen);
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
}

void CallController::FillRingPricing(const CallScreen& screen) {
  // P001: show waive / take-all when inviter offered a positive initiation amount.
  ring_.show_pricing = screen.offer_minor > 0;
  ring_.accept_charge_enabled = screen.charge_enabled;
  if (!ring_.show_pricing) {
    ring_.pricing_label.clear();
    ring_.accept_charge_label.clear();
    ring_.accept_charge_hint.clear();
    return;
  }
  const std::string amount = std::to_string(screen.offer_minor);
  ring_.pricing_label =
      Tr("call.pricing.offer", {{"amount", amount}, {"currency", kPricingCurrencyDisplayName}}).c_str();
  ring_.accept_charge_label =
      Tr("call.ring.accept_charge", {{"amount", amount}, {"currency", kPricingCurrencyDisplayName}}).c_str();
  ring_.accept_charge_hint = Tr("call.ring.accept_charge_disabled_hint").c_str();
  ring_.accept_label =
      screen.conflict ? Tr("call.ring.end_and_accept_free").c_str() : Tr("call.ring.accept_free").c_str();
}

void CallController::RenderInCall(const CallScreen& screen, const CallMediaScreen& frame, CallUiBackend& backend,
                                  const int64_t now_ms) {
  ClearRing();
  FillInCallFromScreen(screen, now_ms);
  IMediaView& media = backend.Media();
  BindVideoFrameWake(media);
  ApplyLocalMedia(media, frame);
  ApplyVideo(media, backend, frame, now_ms);
  ApplyLevels(media);
  ApplyStatusLine(screen, frame, now_ms);
  ApplyMediaHealth(media, backend, frame, now_ms);
  ApplyConnectStory(frame);
  LogSubtitleChange(screen, backend);
  SyncShellState();
}

void CallController::LogSubtitleChange(const CallScreen& screen, CallUiBackend& backend) {
  const std::string sub = in_call_.subtitle.c_str();
  if (sub == logged_subtitle_) {
    return;
  }
  logged_subtitle_ = sub;
  log().info << "in-call subtitle=\"" << sub << "\" phase=" << CallPhaseName(backend.Phase())
             << " status=" << CallMediaStatusName(backend.MediaStatus())
             << " screen=" << CallScreenStatusName(screen.status)
             << " media_live=" << (backend.MediaChromeLive() ? 1 : 0)
             << " media_active=" << (backend.Media().IsActive() ? 1 : 0);
}

void CallController::FillInCallFromScreen(const CallScreen& screen, const int64_t now_ms) {
  auto& in_call = in_call_;
  in_call.active = true;
  in_call.call_id = screen.call_id;
  in_call.muted = screen.muted;
  in_call.show_speaker = screen.show_speaker;
  in_call.speaker_on = screen.speaker_on;
  in_call.participant_count = static_cast<int>(screen.members.size());
  in_call.show_roster = screen.group;
  in_call.show_invite = screen.show_invite;
  in_call.show_make_group = screen.show_make_group;
  in_call.group_invite_text =
      screen.group_invite
          ? Tr("call.group_invite.banner", {{"name", CallerNameFor(screen.group_invite->maker_identity).label},
                                            {"group", screen.group_invite->group_title}})
                .c_str()
          : "";
  FillGuestActions(in_call, screen);
  FillRoster(in_call, screen);
  ResetChromeForCall(screen.call_id, in_call.show_roster || phone_layout_); // phones: the full call page
  ApplyChromeModeToState(in_call);
  ApplySpotlightToState(in_call);

  if (in_call.show_roster) {
    in_call.title = screen.video ? Tr("call.title.group_video").c_str() : Tr("call.title.group_voice").c_str();
  } else {
    in_call.title = screen.video ? Tr("call.title.video").c_str() : Tr("call.title.voice").c_str();
  }
  const std::string peer_label =
      screen.peer_identity.empty() ? Tr("call.label.them") : CallerNameFor(screen.peer_identity).label;
  in_call.peer_label = in_call.show_roster ? Tr("call.label.others").c_str() : peer_label.c_str();
  in_call.show_retry = screen.show_retry;
  in_call.calling = ShowsCancelCall(screen);
  FillPhoneCallFields(screen, now_ms);
}

void CallController::FillRoster(CallInProgressState& in_call, const CallScreen& screen) {
  in_call.roster.clear();
  in_call.roster.reserve(screen.members.size());
  for (const CallScreen::Member& member : screen.members) {
    CallRosterParticipantState entry;
    entry.is_local = member.is_local;
    const CallerName caller = member.is_local ? CallerName{} : CallerNameFor(member.identity);
    const std::string name = member.is_local ? Tr("call.label.you") : caller.label;
    entry.name = name.c_str();
    entry.identity = member.identity.c_str();
    if (const std::string avatar_path = IconPath(member.identity); !avatar_path.empty()) {
      entry.has_avatar = true;
      entry.avatar_src = avatar_path.c_str();
    }
    entry.avatar_unknown = caller.unknown && !entry.has_avatar;
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
}

void CallController::ResetChromeForCall(const std::string& call_id, const bool full_page) {
  // V031: pick the default once per call; the user's mode holds across ticks.
  if (chrome_mode_call_id_ == call_id) {
    return;
  }
  chrome_mode_ = DefaultCallChromeMode(full_page);
  restore_mode_ = chrome_mode_;
  chrome_mode_call_id_ = call_id;
  spotlight_choice_ = {};
  spotlight_fill_ = true;
  active_speaker_.Reset();
  speaking_shown_.clear();
  speaking_shown_ms_ = 0;
  self_heard_ms_ = 0;
}

std::string CallController::IconPath(const std::string& identity) const {
  return call_ports_.identity_icon_local_path && !identity.empty() ? call_ports_.identity_icon_local_path(identity)
                                                                   : std::string();
}

bool CallController::CallFromChat(const std::string& thread_id, const bool want_video) {
  return presenter_.CallFromChat(thread_id, want_video);
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
    const std::string name = CallerNameFor(member.identity).label;
    CallGuestActionState row;
    row.identity = member.identity.c_str();
    if (!local.empty() && member.brought_by == local) {
      row.line = Tr("call.guest.line_yours", {{"name", name}}).c_str();
    } else {
      row.line = Tr("call.guest.line", {{"name", name}, {"bringer", CallerNameFor(member.brought_by).label}}).c_str();
    }
    if (member.can_add_to_group) {
      row.add_to_group_label = Tr("call.guest.add_to_group", {{"name", name}}).c_str();
    }
    row.can_remove = member.can_remove;
    in_call.guest_actions.push_back(std::move(row));
  }
}

void CallController::RemoveGuestFromCall(const std::string& identity) {
  const std::string name = CallerNameFor(identity).label;
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
  const std::string name = CallerNameFor(identity).label;
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
  local_hold_.OnMuteTapped();
  presenter_.ToggleMute();
}

void CallController::ToggleCamera() {
  if (local_hold_.TakeCameraTap()) {
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

void CallController::ApplyLocalMedia(IMediaView& media, const CallMediaScreen& frame) {
  auto& in_call = in_call_;
  in_call.muted = media.IsMuted();
  in_call.camera_on = media.IsCameraEnabled();
  // The facing actually open (it changes when the other camera has opened, not on the tap): the flip button is lit
  // while the front camera is in use (#83).
  in_call.camera_front = in_call.camera_on && media.CameraFrontFacing();
  SyncCameraRotation(media);
  in_call.show_speaker = AudioSession::SupportsSpeakerToggle();
  in_call.speaker_on = AudioSession::IsSpeakerphoneOn();
  in_call.show_camera = frame.media.show_camera;
  in_call.show_flip = ShowsCameraFlip(Platform::IsMobile(), in_call.show_camera, in_call.camera_on);
  in_call.on_hold = frame.media.on_hold;
}

void CallController::SyncCameraRotation(IMediaView& media) {
  if (in_call_.camera_on && CameraRotationFollowsDevice()) {
    // iOS: video follows the phone's physical orientation (read on UI, per tick).
    const int rotation = CameraDisplayRotationDegrees();
    if (rotation != pushed_camera_rotation_) {
      media.UpdateCameraDisplayRotation(rotation);
      pushed_camera_rotation_ = rotation;
    }
  } else if (!in_call_.camera_on && pushed_camera_rotation_ != -1) {
    StopCameraOrientationTracking(); // camera off: don't keep the motion sensors running
    pushed_camera_rotation_ = -1;
  }
}

void CallController::ApplyVideo(IMediaView& media, CallUiBackend& backend, const CallMediaScreen& frame,
                                const int64_t now_ms) {
  auto& in_call = in_call_;
  in_call.remote_video = frame.media.remote_video;
  if (!in_call.remote_video) {
    CallVideoTileRenderer::Instance().ClearRemote();
  }
  SubmitVideoTiles(media, now_ms);
  SubmitRosterTiles(media);
  ApplySpotlightToState(in_call); // the spotlight / strip copies take the rows' new video flags
  RequestVideoRefreshes(media, backend, frame.media, now_ms);
  in_call.stage_visible = frame.media.stage_visible || in_call.local_preview;
  const char* placeholder = PlaceholderKey(frame.media.placeholder);
  in_call.remote_placeholder = placeholder[0] ? Tr(placeholder).c_str() : "";
}

void CallController::SubmitVideoTiles(IMediaView& media, const int64_t now_ms) {
  auto& in_call = in_call_;
  IMediaView::VideoTileFrame local_tile;
  in_call.local_preview = media.CopyLocalVideoFrame(local_tile);
  bool self_new = false;
  bool peer_new = false;
  if (in_call.local_preview) {
    self_new = local_tile.seq != last_shown_self_seq_;
    last_shown_self_seq_ = local_tile.seq;
    CallVideoTileRenderer::Instance().SubmitLocalFrame(TileFrame(std::move(local_tile)));
  }
  if (IMediaView::VideoTileFrame remote_tile; in_call.remote_video && media.CopyRemoteVideoFrame(remote_tile)) {
    peer_new = remote_tile.seq != last_shown_peer_seq_;
    last_shown_peer_seq_ = remote_tile.seq;
    CallVideoTileRenderer::Instance().SubmitRemoteFrame(TileFrame(std::move(remote_tile)));
  }
  metrics_.NoteVideoFrame(in_call.local_preview, self_new, in_call.remote_video, peer_new, now_ms);
}

void CallController::SubmitRosterTiles(IMediaView& media) {
  std::vector<uint32_t> live_peer_streams;
  for (CallRosterParticipantState& row : in_call_.roster) {
    if (row.is_local) {
      row.has_remote_video = false;
      continue;
    }
    const uint32_t stream = RowStream(row);
    row.has_remote_video = stream != 0 && media.HasRemoteVideoForStream(stream);
    if (!row.has_remote_video) {
      continue;
    }
    live_peer_streams.push_back(stream);
    if (IMediaView::VideoTileFrame peer_tile; media.CopyRemoteVideoFrameForStream(stream, peer_tile)) {
      CallVideoTileRenderer::Instance().SubmitPeerFrame(stream, TileFrame(std::move(peer_tile)));
    }
  }
  CallVideoTileRenderer::Instance().RetainPeers(live_peer_streams);
}

void CallController::RequestVideoRefreshes(IMediaView& media, CallUiBackend& backend, const CallMediaDecision& media_d,
                                           const int64_t now_ms) {
  const std::string& call_id = presenter_.ActiveCallId();
  if (call_id.empty()) {
    return;
  }
  // A decoder that failed (or a first gap) asks its sender for a key frame.
  for (const uint32_t stream : media.TakePendingVideoRefreshStreamIds()) {
    for (const CallRosterParticipantState& row : in_call_.roster) {
      if (!row.is_local && RowStream(row) == stream) {
        backend.RequestVideoRefresh(call_id, row.identity.c_str());
        break;
      }
    }
  }
  if (!(media_d.video_missing || media_d.remote_stalling) || now_ms - last_video_refresh_ms_ < 2000) {
    return;
  }
  // A stalled or missing picture: ask for a key frame at most every 2 s.
  last_video_refresh_ms_ = now_ms;
  if (in_call_.show_roster) {
    for (const CallRosterParticipantState& row : in_call_.roster) {
      if (!row.is_local && row.video_enabled && !row.has_remote_video) {
        backend.RequestVideoRefresh(call_id, row.identity.c_str());
      }
    }
  } else if (auto peer = backend.PeerIdentityForCall(call_id); peer && peer->has_value()) {
    backend.RequestVideoRefresh(call_id, **peer);
  }
}

void CallController::ApplyLevels(IMediaView& media) {
  auto& in_call = in_call_;
  const bool muted = in_call.muted;
  in_call.mic_level = muted ? 0 : QuantizeAudioLevel(media.LocalInputLevel());
  in_call.peer_level = QuantizeAudioLevel(media.RemoteOutputLevel());
  if (!media.HasLocalCapture() && !muted) {
    in_call.mic_hint = Tr("call.level.silent").c_str();
  } else {
    in_call.mic_hint = LevelHint(in_call.mic_level, false, muted).c_str();
  }
  in_call.peer_hint = LevelHint(in_call.peer_level, true, false).c_str();
}

void CallController::ApplyStatusLine(const CallScreen& screen, const CallMediaScreen& frame, const int64_t now_ms) {
  auto& in_call = in_call_;
  in_call.elapsed = FormatTimer(frame.timer_since_ms, now_ms).c_str();
  in_call.subtitle = StatusLineText(frame.line, screen, now_ms).c_str();
  in_call.status_hint = StatusHintText(frame.line.hint, screen).c_str();
  in_call.quality_hint = frame.hint_shown ? Tr(frame.hint_key).c_str() : "";
}

std::string CallController::StatusLineText(const CallStatusLine& line, const CallScreen& screen,
                                           const int64_t now_ms) const {
  using Kind = CallStatusLine::Kind;
  std::string text;
  switch (line.kind) {
    case Kind::Words:
      text = Tr(line.key);
      break;
    case Kind::Timer:
      text = FormatTimer(line.since_ms, now_ms);
      if (text.empty() && !line.key.empty()) {
        text = Tr(line.key);
      }
      break;
    case Kind::Activity:
      text = screen.activity;
      break;
    case Kind::Countdown:
      text = Tr(line.key, {{"time", FormatMinSec((line.until_ms - now_ms + 999) / 1000)}});
      break;
    case Kind::Participants:
      text = Tr(line.key, {{"count", std::to_string(line.count)}, {"elapsed", FormatTimer(line.since_ms, now_ms)}});
      break;
    case Kind::PeerOnHold: {
      const std::string name = DisplayNameForIdentity(screen.peer_identity);
      text = name.empty() ? Tr(line.fallback_key) : Tr(line.key, {{"name", name}});
      break;
    }
  }
  // Phones say how the call goes in plain words (the connection story); the path word stays on desktop.
  if (!phone_layout_ && !line.path_key.empty()) {
    const std::string path_label = Tr(line.path_key);
    if (text.empty()) {
      text = path_label;
    } else if (text != path_label) {
      text += " · " + path_label;
    }
  }
  return text;
}

std::string CallController::StatusHintText(const CallScreen::Hint hint, const CallScreen& screen) {
  switch (hint) {
    case CallScreen::Hint::GroupMedia:
      return ComposeGroupCallStatusHint();
    case CallScreen::Hint::DirectConnect:
      return ComposeP2pStatusHint(screen.hint_missing_mic, screen.hint_seed_unreachable);
    case CallScreen::Hint::None:
      break;
  }
  return {};
}

void CallController::ApplyMediaHealth(IMediaView& media, CallUiBackend& backend, const CallMediaScreen& frame,
                                      const int64_t now_ms) {
  auto& in_call = in_call_;
  const CallMediaHealthView& view = frame.health;
  NoteActiveSpeaker(view.engine, in_call.muted ? 0.f : media.LocalInputLevel(), now_ms);
  NoteMediaMetrics(backend, frame, now_ms);
  in_call.quality_bars = view.quality_bars;
  in_call.quality_ok = view.quality <= CallPathQuality::Good;
  in_call.quality_warn = view.quality == CallPathQuality::Fair;
  in_call.quality_error = view.quality == CallPathQuality::Poor || view.quality == CallPathQuality::NoAudio ||
                          view.quality == CallPathQuality::Reconnecting;
  const char* label_key = CallPathQualityLabelKey(view.quality);
  in_call.quality_label = label_key && label_key[0] ? Tr(label_key).c_str() : "";
  ApplyDebugSubtitle(media, view, now_ms);
  if (now_ms - last_media_health_log_ms_ >= 2000) {
    last_media_health_log_ms_ = now_ms;
    log().info << FormatMediaHealthLogLine(view, now_ms, presenter_.ActiveCallId());
  }
  // Toast only real, lasting audio faults (Kenneth 2026-09-28: the old toasts fired on every dip — "Poor" on a jitter
  // spike, "can't hear the other side" in the second before the first audio arrived). Network quality stays visible
  // in the signal bars; an audio fault must persist for CallAudioFaultToastGate::kAudioFaultToastMs and toasts at
  // most once per call.
  const bool audio_fault = view.quality == CallPathQuality::NoAudio || view.asymmetry != CallAudioAsymmetry::None;
  if (audio_fault_gate_.Update(presenter_.ActiveCallId(), audio_fault, now_ms)) {
    if (!frame.hint_key.empty()) {
      // The phone layout already shows this hint as the centered line; don't say it twice. The gate stays consumed.
      if (CallAudioFaultToastGate::ShouldToastHint(frame.hint_shown, in_call.status_hint.empty())) {
        UserFeedback::Fail(Tr(frame.hint_key));
      }
    } else if (label_key && label_key[0]) {
      UserFeedback::Fail(Tr(label_key));
    }
  }
}

void CallController::NoteMediaMetrics(CallUiBackend& backend, const CallMediaScreen& frame, const int64_t now_ms) {
  if (!metrics_.Tracking() || now_ms - last_metrics_media_ms_ < 500) {
    return;
  }
  last_metrics_media_ms_ = now_ms;
  CallMetricsTracker::MediaTick tick;
  tick.connected = backend.MediaChromeLive();
  tick.path = frame.health.path_kind;
  tick.quality = frame.health.quality;
  tick.health = frame.health.engine;
  EmitMetrics(metrics_.NoteMedia(presenter_.ActiveCallId(), tick, now_ms, Vitals(now_ms)));
}

void CallController::ApplyDebugSubtitle(IMediaView& media, const CallMediaHealthView& view, const int64_t now_ms) {
  auto& in_call = in_call_;
  const bool diagnostics = call_ports_.call_diagnostics_enabled && call_ports_.call_diagnostics_enabled();
  in_call.show_debug_subtitle = diagnostics && media.IsActive();
  if (!in_call.show_debug_subtitle) {
    in_call.debug_subtitle = "";
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (in_call.debug_subtitle.empty() || now - last_debug_subtitle_ >= kDebugSubtitleInterval) {
    in_call.debug_subtitle = FormatCallDebugSubtitle(view, now_ms).c_str();
    last_debug_subtitle_ = now;
  }
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
    FillStoryLines(MediaShown().story);
    SyncShellState();
    return;
  }
  BindToMessaging();
  auto* backend = Backend();
  if (!backend || !backend->Available() || !in_call_.active) {
    return;
  }
  const CallMediaScreen& frame = presenter_.MediaScreen();
  const bool diagnostics = call_ports_.call_diagnostics_enabled && call_ports_.call_diagnostics_enabled();
  const std::string body = FormatCallDetailsText(frame.health, util::NowUnixMs(), diagnostics, DetailsCopy(frame));
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

CallDetailsCopy CallController::DetailsCopy(const CallMediaScreen& frame) const {
  const auto& in_call = in_call_;
  const CallMediaHealthView& view = frame.health;
  CallDetailsCopy copy;
  copy.elapsed = in_call.elapsed.empty() ? std::string(in_call.subtitle.c_str()) : std::string(in_call.elapsed.c_str());
  const char* path_key = CallPathWordKey(view.path_kind);
  copy.path_label = Tr(path_key[0] ? path_key : "call.details.path.direct");
  copy.quality_label = Tr(CallPathQualityDetailsLabelKey(view.quality));
  copy.mic_label = LevelHint(in_call.mic_level, false, in_call.muted);
  copy.incoming_label = LevelHint(in_call.peer_level, true, false);
  if (!frame.hint_key.empty()) {
    copy.asymmetry_hint = Tr(frame.hint_key);
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
  return copy;
}

bool CallController::RenderDemoScene(const int64_t now_ms) {
  static const std::pair<std::string, int64_t> spec = []() {
    const char* env = std::getenv("PP_DEMO_CALL");
    return env && env[0] ? ParseDemoCallScene(env) : std::pair<std::string, int64_t>{};
  }();
  if (spec.first.empty()) {
    return false;
  }
  if (demo_started_ms_ == 0) {
    demo_started_ms_ = now_ms - spec.second;
  }
  if (demo_ended_) {
    return true;
  }
  const DemoCallFrame f = DemoCallSceneAt(spec.first, now_ms - demo_started_ms_, now_ms);
  if (!f.valid) {
    return false;
  }
  phone_layout_ = shell_call_chrome_.phone_layout && shell_call_chrome_.phone_layout();
  if (f.call_back) {
    // V064: the call taken is over; its place offers the call End & accept ended back (once).
    if (!demo_call_back_offered_) {
      demo_call_back_offered_ = true;
      ClearInCall();
      ClearRing();
      SyncShellState();
      OfferCallBack(*f.call_back);
    }
    return true;
  }
  switch (f.screen.kind) {
    case CallScreen::Kind::Ringing:
      RenderRing(f.screen, now_ms);
      ringtone_.Stop();
      return true;
    case CallScreen::Kind::Answering:
      RenderAnswering(f.screen, demo_media_.StepAnswering(f.screen, now_ms), now_ms);
      ringtone_.Stop();
      return true;
    case CallScreen::Kind::InCall:
      RenderDemoInCall(f, now_ms);
      return true;
    case CallScreen::Kind::None:
      break;
  }
  return false;
}

void CallController::RenderDemoInCall(const DemoCallFrame& f, const int64_t now_ms) {
  ClearRing();
  // The scene's made-up media through the presenter's own decisions (status line, hold, reconnect, story).
  const CallMediaScreen& frame = demo_media_.Step(f.screen, DemoCallMediaFacts(f, now_ms), DemoCallHealth(f));
  FillInCallFromScreen(f.screen, now_ms);
  in_call_.stage_visible = f.video_stage;
  in_call_.show_camera = true;
  in_call_.camera_on = f.video_stage;
  in_call_.camera_front = f.video_stage;
  in_call_.show_flip = f.video_stage;
  in_call_.remote_placeholder = "";
  in_call_.on_hold = frame.media.on_hold;
  ApplyStatusLine(f.screen, frame, now_ms);
  ApplyConnectStory(frame);
  MediaEngineHealth engine; // the scene's made-up levels, for the speaking rings and the spotlight
  for (const auto& [identity, level] : f.levels) {
    for (const CallRosterParticipantState& row : in_call_.roster) {
      if (identity == row.identity.c_str()) {
        engine.streams.push_back({.stream_id = RowStream(row), .peak_level = level});
      }
    }
  }
  NoteActiveSpeaker(engine, f.self_level, now_ms);
  if (f.video_stage) {
    SubmitDemoCameras(f.screen.status == CallScreen::Status::Connected && !frame.media.on_hold, now_ms);
  }
  SyncShellState();
}

void CallController::SubmitDemoCameras(const bool peer_shown, const int64_t now_ms) {
  // Stand-in cameras, ours blue and theirs warm, so the 1:1 swap shows on the simulator.
  in_call_.local_preview = true;
  in_call_.remote_video = peer_shown;
  if (now_ms - demo_frames_ms_ < 1000) {
    return;
  }
  const uint64_t seq = static_cast<uint64_t>(now_ms);
  static constexpr uint8_t kSelfTop[3] = {40, 90, 200}, kSelfBottom[3] = {20, 30, 70};
  static constexpr uint8_t kPeerTop[3] = {230, 140, 60}, kPeerBottom[3] = {120, 40, 90};
  CallVideoTileRenderer::Instance().SubmitLocalFrame(DemoCameraFrame(seq, kSelfTop, kSelfBottom));
  if (peer_shown) {
    CallVideoTileRenderer::Instance().SubmitRemoteFrame(DemoCameraFrame(seq, kPeerTop, kPeerBottom));
  } else {
    CallVideoTileRenderer::Instance().ClearRemote();
  }
  demo_frames_ms_ = now_ms;
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
                                                const CallScreen::EndingCall& ending, const int64_t now_ms) {
  CallConflictNames names;
  names.caller = caller_label;
  if (!conflict) {
    return names;
  }
  names.same_peer = ending.same_peer;
  names.ending_group = ending.group;
  // A group's call: the group's name; a 1:1 call: its peer's.
  names.ending = ending.group ? GroupTitle(ending.thread_id, now_ms) : DisplayNameForIdentity(ending.peer_identity);
  return names;
}

std::string CallController::GroupTitle(const std::string& thread_id, const int64_t now_ms) {
  if (thread_id.empty() || !call_ports_.get_thread) {
    return {};
  }
  // Read again every kGroupTitleReadMs (a rename shows; a thread not readable yet is tried again), per thread: a ring
  // over a call reads both the ringing group's name and the ending one's.
  GroupTitleRead& read = group_titles_[thread_id];
  if (!read.title.empty() && now_ms - read.read_ms < kGroupTitleReadMs) {
    return read.title;
  }
  read.read_ms = now_ms;
  read.title.clear();
  if (auto thread = call_ports_.get_thread(thread_id); thread && *thread) {
    read.title = (*thread)->local_title.empty() ? (*thread)->title : (*thread)->local_title;
  }
  return read.title;
}

void CallController::FillRingPhoneFields(const CallScreen& screen, const std::string& caller_label,
                                         const int64_t now_ms) {
  ring_.group = screen.ring_group;
  if (screen.ring_group) {
    std::string title = GroupTitle(screen.thread_id, now_ms);
    ring_.phone_title = (title.empty() ? caller_label : title).c_str();
    ring_.phone_subtitle =
        Tr(screen.ring_video ? "call.ring.phone.group_video" : "call.ring.phone.group_voice", {{"name", caller_label}})
            .c_str();
    ring_.has_avatar = false;
    ring_.avatar_src = "";
    ring_.avatar_letter = "";
    ring_.avatar_tone = 2;
    ring_.caller_unknown = false;
    FillRingSteps(false, ring_.phone_title.c_str());
    FillRingSelfAvatar();
    return;
  }
  ring_.phone_title = caller_label.c_str();
  ring_.phone_subtitle = Tr(screen.ring_video ? "call.ring.phone.video" : "call.ring.phone.voice").c_str();
  const AvatarGlyph glyph = MakeAvatarGlyph(caller_label, screen.caller_identity);
  ring_.avatar_letter = glyph.letter.c_str();
  ring_.avatar_tone = glyph.tone;
  const std::string photo = IconPath(screen.caller_identity);
  ring_.has_avatar = !photo.empty();
  ring_.avatar_src = photo.c_str();
  ring_.caller_unknown = CallerNameFor(screen.caller_identity).unknown && photo.empty();
  FillRingSteps(false, caller_label);
  FillRingSelfAvatar();
}

namespace {

/** Our avatar fields, the same names on the ring and the in-call state. */
template <class State>
void SetSelfAvatar(State& state, const AvatarGlyph& glyph, const std::string& photo) {
  state.self_avatar_letter = glyph.letter.c_str();
  state.self_avatar_tone = glyph.tone;
  state.self_has_avatar = !photo.empty();
  state.self_avatar_src = photo.c_str();
}

std::string RingStepText(const CallRingStep& step) {
  return step.key.empty() ? step.text : Tr(step.key);
}

} // namespace

void CallController::FillRingSteps(const bool awaiting_invite, const std::string& caller) {
  const auto steps = CallRingSteps(awaiting_invite, caller);
  ring_.step1 = RingStepText(steps[0]).c_str();
  ring_.step2 = RingStepText(steps[1]).c_str();
  ring_.step3 = RingStepText(steps[2]).c_str();
  ring_.step2_current = steps[1].state == CallRingStep::State::Current;
  ring_.step3_done = steps[2].state == CallRingStep::State::Done;
}

void CallController::FillRingSelfAvatar() {
  // No local identity before the unlock: the letter of "You" and tone 0, replaced in place once it is readable.
  const std::string self =
      call_ports_.local_relay_identity ? call_ports_.local_relay_identity().value_or(std::string{}) : std::string{};
  SetSelfAvatar(ring_, MakeAvatarGlyph(Tr("call.label.you"), self), IconPath(self));
}

void CallController::FillPhoneCallFields(const CallScreen& screen, const int64_t now_ms) {
  auto& in_call = in_call_;
  const bool answering = screen.kind == CallScreen::Kind::Answering;
  const bool group = answering ? screen.ring_group : screen.group;
  const std::string& peer = answering ? screen.caller_identity : screen.peer_identity;
  in_call.video = answering ? screen.answered_video : screen.video;
  const CallerName caller = peer.empty() ? CallerName{.label = {}, .unknown = false} : CallerNameFor(peer);
  const std::string& name = caller.label;
  if (group) {
    const std::string title = GroupTitle(screen.thread_id, now_ms);
    in_call.phone_title = title.empty() ? in_call.title : title.c_str();
  } else {
    in_call.phone_title =
        name.empty() ? Tr("call.label.them").c_str() : name.c_str(); // a peer unknown by name reads "Stranger"
  }
  const AvatarGlyph peer_glyph = MakeAvatarGlyph(name, peer);
  in_call.peer_avatar_letter = peer_glyph.letter.c_str();
  in_call.peer_avatar_tone = peer_glyph.tone;
  const std::string peer_photo = IconPath(peer);
  in_call.peer_has_avatar = !peer_photo.empty();
  in_call.peer_avatar_src = peer_photo.c_str();
  in_call.peer_unknown = !group && caller.unknown && peer_photo.empty();
  FillSelfAvatar(screen);

  // Couldn't connect / disconnected: the failure page says it plainly; why is in the log (#31 / #37).
  using Status = CallScreen::Status;
  in_call.phone_failed =
      !answering && (screen.status == Status::CouldntConnect || screen.status == Status::Disconnected);
  if (screen.status == Status::Disconnected) {
    in_call.failed_text = Tr("call.status.disconnected").c_str();
  } else if (group || screen.hint_missing_mic) {
    in_call.failed_text = Tr("call.status.couldnt_connect").c_str();
  } else {
    in_call.failed_text = Tr("call.error.peer_unreachable").c_str();
  }
  in_call.story_sheet_open = story_sheet_open_ && !in_call.phone_failed;
}

void CallController::FillSelfAvatar(const CallScreen& screen) {
  std::string self;
  for (const CallScreen::Member& member : screen.members) {
    if (member.is_local) {
      self = member.identity;
    }
  }
  if (self.empty() && call_ports_.local_relay_identity) {
    self = call_ports_.local_relay_identity().value_or(std::string{});
  }
  SetSelfAvatar(in_call_, MakeAvatarGlyph(Tr("call.label.you"), self), IconPath(self));
}

void CallController::ApplyConnectStory(const CallMediaScreen& frame) {
  if (!phone_layout_) {
    return; // desktop keeps its bar as it was
  }
  const CallStoryView& v = frame.story;
  in_call_.story_active = v.connecting && !in_call_.phone_failed;
  RenderStorySteps(v);
  RenderStorySignal(v);
  RenderReconnectCapsule(frame.reconnect);
  if (story_sheet_open_) {
    FillStoryLines(v);
  }
}

void CallController::RenderStorySteps(const CallStoryView& v) {
  auto& in_call = in_call_;
  const std::string name = in_call.phone_title.c_str();
  const CallStorySteps steps = CallStoryLines(v, in_call.show_roster);
  in_call.story_step_count = static_cast<int>(steps.keys.size());
  in_call.story_stage = steps.shown;
  in_call.story_inbound = !v.outgoing;
  // A 1:1 answerer's list starts with the three done lines the ring page showed (named by the answering page's own
  // title: the ring page's state is gone by now). The video card does not carry them.
  in_call.story_lead.clear();
  in_call.story_scroll_rows = 0;
  in_call.story_area_dp = 0;
  if (!v.outgoing && !in_call.show_roster && !in_call.stage_visible) {
    for (const CallRingStep& lead : CallRingSteps(false, name)) {
      in_call.story_lead.push_back(RingStepText(lead));
    }
    const int rows = static_cast<int>(in_call.story_lead.size() + steps.keys.size());
    const int reached = std::min(rows, static_cast<int>(in_call.story_lead.size()) + steps.shown + 1);
    const int page_dp = shell_call_chrome_.call_page_height_dp ? shell_call_chrome_.call_page_height_dp() : 0;
    in_call.story_area_dp = page_dp > 0 ? std::max(0, CallStepAreaHeightDp(page_dp)) : 0;
    in_call.story_scroll_rows = CallStepScrollRows(reached, in_call.story_area_dp);
  }
  ui::String* const shown[] = {&in_call.story_step1, &in_call.story_step2, &in_call.story_step3, &in_call.story_step4,
                               &in_call.story_step5};
  for (size_t i = 0; i < std::size(shown); ++i) {
    *shown[i] = i < steps.keys.size() ? Tr(steps.keys[i], {{"name", name}}).c_str() : "";
  }
}

void CallController::RenderStorySignal(const CallStoryView& v) {
  auto& in_call = in_call_;
  in_call.result_text = v.result_shown ? Tr("call.story.result", {{"how", Tr(CallRouteResultKey(v.result_route))},
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
  in_call.notice_fading = v.notice_fading;
}

void CallController::RenderReconnectCapsule(const CallReconnectView& rc) {
  const char* key = CallReconnectKey(rc.kind);
  in_call_.reconnect_text = key[0] ? Tr(key).c_str() : "";
  in_call_.reconnect_ok = rc.kind == CallReconnectKind::Restored;
  in_call_.reconnect_fading = rc.fading;
  in_call_.reconnect_dim = rc.timer_dim;
}

void CallController::FillStoryLines(const CallStoryView& v) {
  auto& in_call = in_call_;
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
      line.time = FormatMinSec(event.call_ms / 1000); // after connect: on the call's clock
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

} // namespace pbr
