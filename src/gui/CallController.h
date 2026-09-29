#pragma once

#include "common/media/CallMediaHealth.h"
#include "domain/media/CallRingtone.h"
#include "domain/ui/ShellTypes.h"
#include "common/Module.h"
#include "feature/calls/CallFunctionalPorts.h"
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

/** Shell-level call ring / in-call chrome. */
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
  bool StartCallDirect(const std::string& thread_id, bool video_allowed);
  /** Shared AcceptIncoming/AcceptIncomingVoiceOnly body (charge decision is always Waive here). */
  void AcceptIncomingImpl(bool voice_only);
  void SyncShellState();
  void ClearRing();
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

  bool MessagingInitialized() const;
  CallUiBackend* Backend();

  bool pending_call_wake_notify_ = false;
  /** Last CallSessionManager identity we installed OnRingChanged on (recreated across unlock). */
  const void* bound_calls_ = nullptr;
  std::string ringing_call_id_;
  std::string last_ring_call_id_;
  std::string active_call_id_;
  int64_t ring_started_ms_ = 0;
  int64_t last_pulse_toggle_ms_ = 0;
  int64_t last_media_health_log_ms_ = 0;
  int last_warned_quality_ = -1;
  /** Display rotation last pushed to the engine while the camera is on (-1: none). */
  int pushed_camera_rotation_ = -1;
  int64_t last_video_refresh_ms_ = 0;
  /** One-shot guard: camera already turned off because video_allowed narrowed mid-call. */
  bool camera_sync_off_done_ = false;
  /**
   * 2026-09-28: video calls start with the local camera on (supersedes V009's old "join with
   * camera off" default) — set once by the caller (video StartCall) or the callee (non-voice-only
   * accept of a video call); consumed (camera enabled, flag cleared) on the first Tick where the
   * call is connected. Cleared on voice-only accept and on ClearInCall.
   */
  bool auto_camera_pending_ = false;
  /** Call id auto_camera_pending_ applies to. */
  std::string auto_camera_call_id_;
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
  /** Call intents report on UI later (calls owner); results for a destroyed controller are dropped. */
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
  /** Run `fn` only while this controller lives (captured into async call-intent results). */
  template <typename Fn>
  auto WhileAlive(Fn fn) {
    return [weak = std::weak_ptr<int>(alive_), fn = std::move(fn)](auto&&... args) {
      if (weak.lock()) {
        fn(std::forward<decltype(args)>(args)...);
      }
    };
  }
};

} // namespace pbr
