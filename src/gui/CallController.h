#pragma once

#include "gui/CallActiveSpeaker.h"
#include "gui/CallSpotlight.h"
#include "gui/CallAudioFaultToastGate.h"
#include "feature/presenters/CallLocalHold.h"
#include "gui/CallMetricsTracker.h"
#include "gui/UiLatencyProbe.h"
#include "common/media/CallMediaHealth.h"
#include "lib/call_audio/CallRingtone.h"
#include "lib/ui/CallConflictCopy.h"
#include "lib/ui/ShellTypes.h"
#include "common/Module.h"
#include "feature/calls/CallFunctionalPorts.h"
#include "feature/presenters/CallMediaFrame.h"
#include "feature/presenters/CallPresenter.h"
#include "feature/presenters/CallRingSteps.h"
#include "feature/presenters/CallSelfAvatarWatch.h"
#include "feature/presenters/CallSystemUiBridge.h"
#include "gui/CallChromeSync.h"
#include "gui/CallDemoScenes.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "gui/shell/ShellCallChromePorts.h"

#include <unordered_map>
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
  ~CallController();
  CallController(const CallController&) = delete;
  CallController& operator=(const CallController&) = delete;

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
  /** Opens a chat; used to land on the ringing call's thread after a call-alert tap. */
  void BindOpenThread(std::function<void(const std::string& thread_id)> callback);

  void BindToMessaging();
  void Tick();
  /** After inbox sync / call_wake — fetch pending invites and show ring if needed. */
  void RefreshPendingRing();
  void OnCallWake();
  /** Messaging is up (vault unlocked): a call answered on the phone's call screen meanwhile now finds its ring. */
  void OnMessagingReady();
  /** Join ringtone before Backend::Shutdown / SDL_Quit (accept-dialog quit hang). */
  void PrepareForShutdown();

  /** A chat calls (its call button, a tapped call record, a call-back): CallPresenter::CallFromChat. */
  bool CallFromChat(const std::string& thread_id, bool want_video);
  /** A group call goes on in this chat that this device may join (V053): the call button joins it. */
  bool CallJoinableInThread(const std::string& thread_id);
  /** The group chat's call bar ("A voice call is on · 3 people"); "" when there is no call to join. */
  std::string CallJoinBarText(const std::string& thread_id);
  /** Start with explicit invitee relay identities (group / picker flow). */
  bool StartCallWithInvitees(const std::string& thread_id, bool video_allowed,
                             const std::vector<std::string>& invitee_identities);
  void OpenGroupCallPicker(const std::string& thread_id, bool video_allowed);
  void OpenMidCallInvitePicker();
  /** G022: "Make a group…" and the group banner's Join / Not now (CallPresenter decides). */
  void OpenMakeGroup();
  void JoinGroupInvite();
  void PutOffGroupInvite();
  /** V062 3: "Remove from call…" on a guest: asks, then takes them out of the call. */
  void RemoveGuestFromCall(const std::string& identity);
  /** V062 4: "Add {name} to the group" on a guest. */
  void AddGuestToGroup(const std::string& identity);
  /** V062 2: the add-people picker offers `identity` for the active call. */
  bool MayAddToCall(const std::string& identity) const { return presenter_.MayAddToActiveCall(identity); }
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
  /** V059: spotlight a roster row (its stream_id) in Immersive; back to the grid; toggle fill / fit. */
  void SpotlightParticipant(const std::string& stream_id);
  void SpotlightGrid();
  void ToggleSpotlightFill();
  void RestoreChromeFromMinimized();
  /** #166: the minimised chip was dropped: its side (1 left, 2 right) and height in the band (0..1000). */
  void SetMinimizedPlace(int side, int y_permille, bool tucked = false);
  CallChromeMode ChromeMode() const { return chrome_mode_; }
  /** Thin Call details sheet (debug numbers when diagnostics enabled). */
  void ShowCallDetails();

private:
  // --- Rendering: the presenter's screen and media frame into the shell's ring / in-call state. ---------------------
  void Render();
  void RenderRing(const CallScreen& screen, int64_t now_ms);
  /**
   * A tapped call alert's page (phone layout): the ring page with "Incoming call" and its three lines on their
   * way in place of the caller. CallPresenter::StepWakePlaceholder decides; this only shows / clears ring_ and emits the metric.
   */
  void StepWakePlaceholder();
  void RenderAwaitingRing();
  void EmitWakeRingMetric(const CallPresenter::WakeStep& view);
  /** The ring's answer buttons: video / voice, free / take-all (P001), the conflict's End & accept (V064). */
  void FillRingAnswerLabels(const CallScreen& screen);
  void FillRingPricing(const CallScreen& screen);
  /** Accept in flight: the ring is gone, a Connecting page / bar shows. */
  void RenderAnswering(const CallScreen& screen, const CallMediaScreen& frame, int64_t now_ms);
  void RenderInCall(const CallScreen& screen, const CallMediaScreen& frame, CallUiBackend& backend, int64_t now_ms);
  /** The in-call state from the presenter's screen (names, roster, buttons, chrome mode, phone page fields). */
  void FillInCallFromScreen(const CallScreen& screen, int64_t now_ms);
  void FillRoster(CallInProgressState& in_call, const CallScreen& screen);
  /** V062: the guests this device may remove or invite to the group, worded for the call chrome. */
  void FillGuestActions(CallInProgressState& in_call, const CallScreen& screen);
  /** The phone call page's names, avatars and failure line (ShellCallChromeRml). */
  void FillPhoneCallFields(const CallScreen& screen, int64_t now_ms);
  void FillSelfAvatar(const CallScreen& screen);
  /** The ring page's three lines (CallRingSteps) written into ring_; `caller` is the name or group title shown. */
  void FillRingSteps(bool awaiting_invite, const std::string& caller, CallRingMedia media);
  /** Our own avatar on the ring page's left: the photo / letter of the local identity, else "You" and a neutral tone. */
  void FillRingSelfAvatar();
  /** The local identity ("" before the unlock). */
  std::string LocalIdentity() const;
  /** Whether the ring page has room for its three lines (CallRingShowsSteps), from ring_ and the page height now. */
  void FillRingShowSteps();
  void FillRingPhoneFields(const CallScreen& screen, const std::string& caller_label, int64_t now_ms);
  /** Mute, camera (its buttons, rotation), speaker and the hold, from the engine and the frame. */
  void ApplyLocalMedia(IMediaView& media, const CallMediaScreen& frame);
  void SyncCameraRotation(IMediaView& media);
  /** The video stage, its placeholder and the tiles' pictures; key frames asked for a stalled picture. */
  void ApplyVideo(IMediaView& media, CallUiBackend& backend, const CallMediaScreen& frame, int64_t now_ms);
  void SubmitVideoTiles(IMediaView& media, int64_t now_ms);
  /** Group calls: each roster row's picture (and its has_remote_video flag). */
  void SubmitRosterTiles(IMediaView& media);
  void RequestVideoRefreshes(IMediaView& media, CallUiBackend& backend, const CallMediaDecision& media_d,
                             int64_t now_ms);
  /** The mic / incoming meters and their words. */
  void ApplyLevels(IMediaView& media);
  /** The frame's status line, timer, hint lines — worded with the render's clock. */
  void ApplyStatusLine(const CallScreen& screen, const CallMediaScreen& frame, int64_t now_ms);
  std::string StatusLineText(const CallStatusLine& line, const CallScreen& screen, int64_t now_ms) const;
  static std::string StatusHintText(CallScreen::Hint hint, const CallScreen& screen);
  /** The quality chip, the debug subtitle, the media metrics and log, and a lasting audio fault's toast. */
  void ApplyMediaHealth(IMediaView& media, CallUiBackend& backend, const CallMediaScreen& frame, int64_t now_ms);
  void NoteMediaMetrics(CallUiBackend& backend, const CallMediaScreen& frame, int64_t now_ms);
  void ApplyDebugSubtitle(IMediaView& media, const CallMediaHealthView& view, int64_t now_ms);
  /** Phones: the frame's connection story (CallConnectStory) and reconnect capsule; desktop is left alone. */
  void ApplyConnectStory(const CallMediaScreen& frame);
  void RenderStorySteps(const CallStoryView& v);
  void RenderStorySignal(const CallStoryView& v);
  void RenderReconnectCapsule(const CallReconnectView& rc);
  /** The "how this call got through" sheet's lines from the story's events. */
  void FillStoryLines(const CallStoryView& v);
  CallDetailsCopy DetailsCopy(const CallMediaScreen& frame) const;
  void LogSubtitleChange(const CallScreen& screen, CallUiBackend& backend);
  void ApplyChromeModeToState(CallInProgressState& in_call);
  /** A new call (or answer) on screen: its default chrome mode, spotlight and speaking rings. */
  void ResetChromeForCall(const std::string& call_id, bool full_page);
  void ApplySpotlightToState(CallInProgressState& in_call);
  void ResyncSpotlight();
  /** Group calls: feed the active speaker each frame; a new one moves the spotlight (unless someone chose). Also
   * the speaking rings: the others by their streams' levels, we by `self_level` (0 when muted). */
  void NoteActiveSpeaker(const MediaEngineHealth& engine, float self_level, int64_t now_ms);
  void SyncShellState();
  void ClearRing();
  void ClearInCall();
  /** Hide in-call bar without clearing active_call_id_ (conflict ring). */
  void HideInCallChrome();

  // --- Words: names, titles, notices. --------------------------------------------------------------------------------
  /** The contact's name or nickname; EMPTY when none is known (never the account id). */
  std::string DisplayNameForIdentity(const std::string& identity) const;
  /** What a call page calls `identity`: the name, or "Stranger" (+ `unknown`, for the silhouette avatar). */
  CallerName CallerNameFor(const std::string& identity) const;
  /** The word for a sentence's `{name}` slot: the name or "Stranger"; EMPTY only when there is no identity at all. */
  std::string PeerLabelFor(const std::string& identity) const;
  std::string IconPath(const std::string& identity) const;
  /** A group's name for the phone page and a ring's words, re-read at most every kGroupTitleReadMs per thread. */
  std::string GroupTitle(const std::string& thread_id, int64_t now_ms);
  /** V064: who a ring over a call names — its caller, and the call answering ends (its peer, or its group). */
  CallConflictNames ConflictNames(const std::string& caller_label, bool conflict, const CallScreen::EndingCall& ending,
                                  int64_t now_ms);
  void ShowNotice(const CallNotice& notice);
  /** V064: "Call {name} back?" once the call taken in its place is over; Call back calls from its chat. */
  void OfferCallBack(const CallScreen::EndingCall& ended);
  /** "m:ss" of the call timer from `since_ms` ("" when there is none). */
  static std::string FormatTimer(int64_t since_ms, int64_t now_ms);
  static std::string FormatMinSec(int64_t seconds);

  // --- Devices: ringtones, the video wake, the hold, the ongoing-call service, metrics. -----------------------------
  void SyncRingtone();
  /**
   * Stop ringback_ if playing and, unless media is already active (the engine then owns
   * the session), release the phone audio session. Shared by SyncRingtone's own
   * playing->stopped transition and LeaveActive's immediate stop, so a hang-up during
   * ringback never Deactivates twice.
   */
  void StopRingback(CallUiBackend* backend);
  /** Once per media view: its new video frames wake the frame loop (at most every kVideoWakeMinIntervalMs). */
  void BindVideoFrameWake(IMediaView& media);
  CallLocalHold::Ports LocalHoldPorts();
  /** Hands the hold the OS's facts (each tick). */
  void SyncLocalHold();
  void OnAppBackground();
  void OnAppForeground();
  /** The call this device is in, as last told to the OS (OngoingCallService: Android's call service). */
  void SyncOngoingCall();
  /** Device vitals, re-read at most every 2 s. */
  const DeviceVitals& Vitals(int64_t now_ms);
  void EmitMetrics(const std::vector<ShareableLine>& lines);
  /** perf.threads: once every 10 s while a call is tracked (where the CPU goes). */
  void EmitPerfThreads(int64_t now_ms);
  CallUiBackend* Backend();

  // --- UI demo app only (kUiDemoBuild). -------------------------------------------------------------------------------
  /** Draws the scripted call PP_DEMO_CALL names; false when none is set. */
  bool RenderDemoScene(int64_t now_ms);
  void RenderDemoInCall(const DemoCallFrame& f, int64_t now_ms);
  /** Stand-in camera pictures for the tiles (the simulator has no camera). */
  void SubmitDemoCameras(bool peer_shown, int64_t now_ms);
  /** An answer / decline / hang-up ends the scripted call. */
  void EndDemoScene();
  /** The frame on screen: the presenter's, or the demo scene's stand-in. */
  const CallMediaScreen& MediaShown() const {
    return demo_started_ms_ != 0 ? demo_media_.View() : presenter_.MediaScreen();
  }

  // ==================================================================================================================
  static constexpr int64_t kDisconnectedCloseMs = 3000;
  static constexpr std::chrono::milliseconds kDebugSubtitleInterval{1000};
  /** The tiles' speaking rings change at most this often (B146: each change dirties only the roster rows). */
  static constexpr int64_t kSpeakingRingRefreshMs = 400;
  static constexpr int64_t kGroupTitleReadMs = 2000;

  // --- What it renders, and the ports it renders through.
  CallPresenter presenter_;
  /** The phone's own call screen (Android's call notification), kept in step with the presenter's. */
  CallSystemUiBridge system_call_screen_;
  CallFunctionalPorts call_ports_;
  PeoplePickerNotifyPorts people_picker_notify_;
  ShellCallChromePorts shell_call_chrome_;
  std::function<void()> peer_link_refresh_;
  std::function<void(const std::string&)> open_thread_;
  /** Presenter-owned call chrome; pushed to ShellHost via apply_snapshot. */
  CallRingState ring_;
  CallInProgressState in_call_;
  /** Last chrome applied — idle poll must not remount when unchanged. */
  CallChromeSyncGate chrome_gate_;
  /** Shell in its phone layout (ShellCallChromePorts::phone_layout), read each render. */
  bool phone_layout_ = false;

  // --- The screen's own state (GUI-only: layout, words already decided).
  /** What the chat header last heard about (it shows "In call"): refreshed when this changes. */
  CallScreen::Kind header_kind_ = CallScreen::Kind::None;
  std::string header_call_id_;
  int64_t last_pulse_toggle_ms_ = 0;
  /** When the screen turned Disconnected (0: not); it closes after kDisconnectedCloseMs. */
  int64_t disconnected_since_ms_ = 0;
  /** The diagnostics subtitle carries a millisecond age, new every frame; it is rebuilt at most once a
   *  second so it does not re-lay the call screen every frame (B146). */
  std::chrono::steady_clock::time_point last_debug_subtitle_{};
  std::string logged_subtitle_;
  /** Lasting audio faults toast once per call (see CallAudioFaultToastGate). */
  CallAudioFaultToastGate audio_fault_gate_;
  /** The story notice of the last sync: the "line got slower" pill fires on the edge into Slow. */
  CallStoryNotice last_story_notice_ = CallStoryNotice::None;
  bool story_sheet_open_ = false;
  /** V031: the chrome mode, the one to restore when leaving Minimized (Expanded or Immersive), and the call they were
   * chosen for (defaults reset on a switch). */
  CallChromeMode chrome_mode_ = CallChromeMode::Expanded;
  CallChromeMode restore_mode_ = CallChromeMode::Expanded;
  std::string chrome_mode_call_id_;
  /** #166: the minimised chip's place this call (ShellTypes.h CallInProgressState::minimized_side). */
  int minimized_side_ = 0;
  int minimized_y_permille_ = 0;
  bool minimized_tucked_ = false;
  /** V059: the user's spotlight choice and the spotlit tile's fill, for the shown call (reset per call). */
  CallSpotlightChoice spotlight_choice_;
  bool spotlight_fill_ = true;
  /** Group calls: who is speaking, for the spotlight when nobody chose (V059). */
  CallActiveSpeaker active_speaker_;
  /** Group calls: the stream_ids whose tile shows the speaking ring (sorted), when that last changed, and when our own
   * microphone last reached CallActiveSpeaker::kSpeakingLevel. */
  std::vector<std::string> speaking_shown_;
  int64_t speaking_shown_ms_ = 0;
  int64_t self_heard_ms_ = 0;
  struct GroupTitleRead {
    std::string title;
    int64_t read_ms = 0;
  };
  std::unordered_map<std::string, GroupTitleRead> group_titles_;

  // --- Devices.
  CallRingtone ringtone_;
  CallRingtone ringback_{CallRingtone::Tone::OutgoingRingback};
  /** V060: the call on hold while the OS has its audio, and the camera's rest (hold, background). */
  CallLocalHold local_hold_;
  uint64_t background_listener_ = 0;
  uint64_t foreground_listener_ = 0;
  bool ongoing_call_on_ = false;
  std::string ongoing_call_title_;
  /** Display rotation last pushed to the engine while the camera is on (-1: none). */
  int pushed_camera_rotation_ = -1;
  /** The media view whose frames wake the loop (BindVideoFrameWake). */
  IMediaView* video_wake_bound_ = nullptr;
  int64_t last_video_refresh_ms_ = 0;
  /** The last self-view / peer frame sent to the screen (call.summary shown_*_fps counts new ones). */
  uint64_t last_shown_self_seq_ = 0;
  uint64_t last_shown_peer_seq_ = 0;

  // --- Metrics and logs.
  /** Operational metrics for the current call (Metrics channel; no identities). */
  CallMetricsTracker metrics_;
  UiLatencyProbe ui_probe_;
  int64_t last_metrics_media_ms_ = 0;
  int64_t last_media_health_log_ms_ = 0;
  DeviceVitals vitals_;
  int64_t vitals_read_ms_ = 0;
  int64_t perf_sample_ms_ = 0;
  double perf_proc_cpu_s_ = -1.0;
  std::unordered_map<uint32_t, double> perf_thread_cpu_;
  /** The same per own thread name, for the developer log (perf.threads.raw). */
  std::unordered_map<uint32_t, double> perf_thread_cpu_raw_;

  // --- UI demo app only.
  /** The scripted call's media frame (the presenter's decisions over made-up facts). */
  CallMediaFrame demo_media_;
  int64_t demo_started_ms_ = 0;
  bool demo_ended_ = false;
  /** The call the ring page last showed and whether it rang as video: the answering page words its lead rows by it. */
  /** The identity the awaiting ring page drew our avatar with (CallSelfAvatarWatch). */
  CallSelfAvatarWatch ring_self_watch_;
  /** The page height FillRingShowSteps last used: Tick decides again only when it differs. */
  int ring_page_height_dp_ = 0;
  std::string rung_call_id_;
  bool rung_video_ = false;
  bool demo_call_back_offered_ = false;
  /** When its stand-in camera pictures were last handed to the tiles. */
  int64_t demo_frames_ms_ = 0;
};

} // namespace pbr
