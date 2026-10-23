#pragma once

#include "gui/CallAudioFaultToastGate.h"
#include "gui/CallMetricsTracker.h"
#include "gui/UiLatencyProbe.h"
#include "common/media/CallMediaHealth.h"
#include "domain/media/CallRingtone.h"
#include "domain/ui/ShellTypes.h"
#include "common/Module.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "feature/calls/CallPresenter.h"
#include "feature/calls/CallSystemUiBridge.h"
#include "gui/CallChromeSync.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "gui/shell/ShellCallChromePorts.h"

#include <memory>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaEngine;
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
  /** Call ring / in-call chrome without ShellHost::Instance(). Clear via BindShellCallChrome({}). */
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
  void RetryConnect();
  void ToggleMute();
  void ToggleCamera();
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
  void ShowNotice(const CallNotice& notice);
  void SyncShellState();
  void ClearRing();
  /** Device vitals, re-read at most every 2 s. */
  const DeviceVitals& Vitals(int64_t now_ms);
  void EmitMetrics(const std::vector<std::string>& lines);
  void ClearInCall();
  /** Hide in-call bar without clearing active_call_id_ (conflict ring). */
  void HideInCallChrome();
  void ApplyChromeModeToState(CallInProgressState& in_call);
  void ApplyAudioLevels(CallMediaEngine& media);
  void RefreshCallLevels();
  void SyncRingtone();
  /**
   * Stop ringback_ if playing and, unless media is already active (the engine then owns
   * the session), release the phone audio session. Shared by SyncRingtone's own
   * playing->stopped transition and LeaveActive's immediate stop, so a hang-up during
   * ringback never Deactivates twice.
   */
  void StopRingback(CallUiBackend* backend);
  void ApplyMediaHealth(CallMediaEngine& media, CallUiBackend* backend, bool media_reconnect);
  CallMediaHealthView BuildMediaHealthView(CallMediaEngine& media, CallUiBackend* backend,
                                           bool media_reconnect) const;
  std::string DisplayNameForIdentity(const std::string& identity) const;
  static std::string FormatElapsed(int64_t connected_at_ms);

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
  /** Peer camera state last logged ("roster/frames"), so the log shows each change once. */
  std::string peer_video_log_key_;
  /** When this video call started waiting for the peer's first frame (0: not yet). */
  int64_t video_start_since_ms_ = 0;
  /** One-shot guard: camera already turned off because video_allowed narrowed mid-call. */
  bool camera_sync_off_done_ = false;
  /** Last chrome applied — idle poll must not remount when unchanged. */
  CallChromeLayer synced_chrome_;
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
};

} // namespace pbr
