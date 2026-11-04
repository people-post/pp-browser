#pragma once

#include <unordered_map>

#include "gui/CallAudioFaultToastGate.h"
#include "gui/CallMetricsTracker.h"
#include "gui/UiLatencyProbe.h"
#include "common/media/CallMediaHealth.h"
#include "lib/call_audio/CallRingtone.h"
#include "lib/ui/ShellTypes.h"
#include "common/Module.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "feature/presenters/CallConnectStory.h"
#include "feature/presenters/CallPresenter.h"
#include "feature/presenters/CallSystemUiBridge.h"
#include "gui/CallChromeSync.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "gui/shell/ShellCallChromePorts.h"

#include <chrono>
#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class IMediaView;
class CallUiBackend;

/**
 * Shell-level call ring / in-call chrome: renders CallPresenter's screen (words, avatars, video,
 * chrome modes) and owns the devices around it (ringtone, ringback, levels, metrics). What the call
 * surface shows and what its buttons do is decided in CallPresenter, which pp-app-probe drives too.
 */
class CallController : public Module {
public:
  CallController();

  void BindCallPorts(CallFunctionalPorts ports);
  /** Open people-picker flows without PeoplePickerController::Instance(). Clear via BindPeoplePickerNotify({}). */
  void BindPeoplePickerNotify(PeoplePickerNotifyPorts ports);
  /** Call ring / in-call chrome. Clear via BindShellCallChrome({}). */
  void BindShellCallChrome(ShellCallChromePorts ports);
  /**
   * Refresh chat peer-link / "In call" header when call chrome changes.
   * Application wires ChatController — no ChatController::Instance().
   */
  void BindPeerLinkRefresh(std::function<void()> callback);

  void BindToMessaging();
  void Tick();
  /** After inbox sync / call_wake — fetch pending invites and show ring if needed. */
  void RefreshPendingRing();
  void OnCallWake();
  /** Join ringtone before Backend::Shutdown / SDL_Quit (accept-dialog quit hang). */
  void PrepareForShutdown();

  bool StartCall(const std::string& thread_id, bool video_allowed);
  /** A group call goes on in this chat that this device may join (V053): the call button joins it. */
  bool CallJoinableInThread(const std::string& thread_id);
  /** The group chat's call bar ("A voice call is on · 3 people"); "" when there is no call to join. */
  std::string CallJoinBarText(const std::string& thread_id);
  /** Start with explicit invitee relay identities (group / picker flow). */
  bool StartCallWithInvitees(const std::string& thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);
  void OpenGroupCallPicker(const std::string& thread_id, bool video_allowed);
  void OpenMidCallInvitePicker();
  void InviteIdentitiesToActiveCall(const std::vector<std::string>& invitee_identities);
  /** Host can encode/send video at all (gates the chat call-type menu's "Video call" item). */
  bool VideoCallAvailable();
  void AcceptIncoming();
  /** Callee narrows this call to voice-only before accepting (video ring only). */
  void AcceptIncomingVoiceOnly();
  /** Take-all when offer > 0; no-op toast when rails unavailable. */
  void AcceptIncomingWithCharge();
  void DeclineIncoming();
  void LeaveActive();
  void EndDisconnectedCall(int64_t now_ms);
  std::string NoticeUserText(const CallNotice& notice) const;
  void RetryConnect();
  void ToggleMute();
  void ToggleCamera();
  void FlipCamera();
  void ToggleSpeaker();

  /** V031 mode controls — remount call chrome mount only. */
  void SetChromeMode(CallChromeMode mode);
  void MinimizeChrome();
  void ExpandChrome();
  void ImmersiveChrome();
  void RestoreChromeFromMinimized();
  void SetMinimizedCorner(int corner);
  CallChromeMode ChromeMode() const { return chrome_mode_; }
  /** Thin Call details sheet (debug numbers when diagnostics enabled). */
  void ShowCallDetails();

private:
  /** The presenter's screen into the shell's ring / in-call state. */
  void Render();
  void RenderRing(const CallScreen& screen);
  void RenderInCall(const CallScreen& screen, CallUiBackend& backend);
  /** The in-call state from the presenter's screen (names, roster, status line, phone page fields). */
  void FillInCallFromScreen(const CallScreen& screen);
  /** UI demo app only (kUiDemoBuild): draws the scripted call PP_DEMO_CALL names; false when none is set. */
  bool RenderDemoScene();
  /** UI demo app only: an answer / decline / hang-up ends the scripted call. */
  void EndDemoScene();
  void ShowNotice(const CallNotice& notice);
  void SyncShellState();
  void ClearRing();
  /** Device vitals, re-read at most every 2 s. */
  const DeviceVitals& Vitals(int64_t now_ms);
  void EmitMetrics(const std::vector<ShareableLine>& lines);
  void ClearInCall();
  /** Hide in-call bar without clearing active_call_id_ (conflict ring). */
  void HideInCallChrome();
  void ApplyChromeModeToState(CallInProgressState& in_call);
  void ApplyAudioLevels(IMediaView& media);
  void RefreshCallLevels();
  void SyncRingtone();
  /**
   * Stop ringback_ if playing and, unless media is already active (the engine then owns
   * the session), release the phone audio session. Shared by SyncRingtone's own
   * playing->stopped transition and LeaveActive's immediate stop, so a hang-up during
   * ringback never Deactivates twice.
   */
  void StopRingback(CallUiBackend* backend);
  void ApplyMediaHealth(IMediaView& media, CallUiBackend* backend, bool media_reconnect);
  CallMediaHealthView BuildMediaHealthView(IMediaView& media, CallUiBackend* backend, bool media_reconnect) const;
  std::string DisplayNameForIdentity(const std::string& identity) const;
  /** The phone call page's names, avatars and failure line (ShellCallChromeRml). */
  void FillPhoneCallFields(const CallScreen& screen);
  void FillRingPhoneFields(const CallScreen& screen, const std::string& caller_label);
  /** Steps the connection story (CallConnectStory) and writes what the phone page shows; desktop is left alone. */
  void ApplyConnectStory(const CallScreen& screen, const std::string& path_kind, CallPathQuality quality,
                         int quality_bars, int64_t now_ms);
  /** The "how this call got through" sheet's lines from the story's events. */
  void FillStoryLines();
  /** A group's name for the phone page (read once per thread). */
  std::string GroupTitle(const std::string& thread_id);
  static std::string FormatElapsed(int64_t connected_at_ms);
  static std::string FormatMinSec(int64_t seconds);

  CallUiBackend* Backend();

  CallPresenter presenter_;
  /** The phone's own call screen (CallKit), kept in step with the presenter's (P013). */
  CallSystemUiBridge system_call_screen_;
  /** What the chat header last heard about (it shows "In call"): refreshed when this changes. */
  CallScreen::Kind header_kind_ = CallScreen::Kind::None;
  std::string header_call_id_;
  int64_t last_pulse_toggle_ms_ = 0;
  /** When the screen turned Disconnected (0: not); it closes after kDisconnectedCloseMs. */
  int64_t disconnected_since_ms_ = 0;
  static constexpr int64_t kDisconnectedCloseMs = 3000;
  int64_t last_media_health_log_ms_ = 0;
  /** The diagnostics subtitle carries a millisecond age, new every frame; it is rebuilt at most once a
   *  second so it does not re-lay the call screen every frame (B146). */
  std::chrono::steady_clock::time_point last_debug_subtitle_{};
  static constexpr std::chrono::milliseconds kDebugSubtitleInterval{1000};
  /** Lasting audio faults toast once per call (see CallAudioFaultToastGate). */
  CallAudioFaultToastGate audio_fault_gate_;
  /** Display rotation last pushed to the engine while the camera is on (-1: none). */
  int pushed_camera_rotation_ = -1;
  int64_t last_video_refresh_ms_ = 0;
  /** Operational metrics for the current call (Metrics channel; no identities). */
  CallMetricsTracker metrics_;
  UiLatencyProbe ui_probe_;
  int64_t last_metrics_media_ms_ = 0;
  /** The call's link, sampled at most every 2 s (resend % is a delta between samples). */
  void SampleLinkHealth(CallUiBackend* backend, int64_t now_ms);
  std::string link_sample_call_id_;
  CallLinkCounters link_counters_;
  CallLinkHealth link_health_;
  int64_t last_link_sample_ms_ = 0;
  DeviceVitals vitals_;
  int64_t vitals_read_ms_ = 0;
  /** perf.threads: once every 10 s while a call is tracked (where the CPU goes). */
  void EmitPerfThreads(int64_t now_ms);
  int64_t perf_sample_ms_ = 0;
  double perf_proc_cpu_s_ = -1.0;
  std::unordered_map<uint32_t, double> perf_thread_cpu_;
  /** The same per own thread name, for the developer log (perf.threads.raw). */
  std::unordered_map<uint32_t, double> perf_thread_cpu_raw_;
  /** Peer camera state last logged ("roster/frames"), so the log shows each change once. */
  std::string peer_video_log_key_;
  /** When this video call started waiting for the peer's first frame (0: not yet). */
  int64_t video_start_since_ms_ = 0;
  /** One-shot guard: camera already turned off because video_allowed narrowed mid-call. */
  bool camera_sync_off_done_ = false;
  /** Last chrome applied — idle poll must not remount when unchanged. */
  CallChromeSyncGate chrome_gate_;
  CallChromeMode chrome_mode_ = CallChromeMode::Expanded;
  /** Mode to restore when leaving Minimized (Expanded or Immersive). */
  CallChromeMode restore_mode_ = CallChromeMode::Expanded;
  int minimized_corner_ = 0;
  /** Call id the current chrome_mode_ was chosen for (reset defaults on switch). */
  std::string chrome_mode_call_id_;
  CallRingtone ringtone_;
  CallRingtone ringback_{CallRingtone::Tone::OutgoingRingback};
  CallFunctionalPorts call_ports_;
  PeoplePickerNotifyPorts people_picker_notify_;
  ShellCallChromePorts shell_call_chrome_;
  std::function<void()> peer_link_refresh_;
  /** Presenter-owned call chrome; pushed to ShellHost via apply_snapshot. */
  CallRingState ring_;
  CallInProgressState in_call_;
  /** Shell in its phone layout (ShellCallChromePorts::phone_layout), read each render. */
  bool phone_layout_ = false;
  CallConnectStory story_;
  std::string story_logged_call_;
  size_t story_events_logged_ = 0;
  bool story_sheet_open_ = false;
  int64_t demo_started_ms_ = 0;
  bool demo_ended_ = false;
  std::string group_title_thread_;
  std::string group_title_;
};

} // namespace pbr
