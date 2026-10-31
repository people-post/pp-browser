#pragma once

#include "gui/chat/ChatScreenState.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/VoiceGesture.h"
#include "lib/media/MicPermission.h"
#include "lib/media/VoiceRecorder.h"
#include "lib/media/VoiceTouchHold.h"
#include "lib/messaging/AttachmentLimits.h"

#include <ui/dom/EventListener.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {
class ElementDocument;
class Event;
} // namespace ui

namespace pbr {

/** Reserved name (not a chat) whose `blobs_view/voice-rec/` holds the recording before it is sent. */
inline constexpr const char* kVoiceViewThread = "voice";

/** What the composer's microphone needs from the chat screen. All called on the UI thread. */
struct VoiceComposerPorts {
  /** The numbers behind a voice message (AttachmentLimits::voice_max_seconds): the recorder is told the limit. */
  std::function<AttachmentLimits()> limits;
  /** Where the temporary recording goes (`.../blobs_view/voice-rec`). */
  std::function<std::string()> out_dir;
  /** The chat that is open now. Read once, when a recording starts: that chat gets the message, whatever is open later. */
  std::function<std::string()> thread_id;
  /** A finished recording to send to `thread_id` as an attachment. The owner deletes the file when it is done with it. */
  std::function<void(const std::string& thread_id, const std::string& path)> send;
  /** This chat can take a voice message (a peer or group chat, messaging ready, nothing else being sent). */
  std::function<bool()> eligible;
  /** The chat model changed. */
  std::function<void()> dirty;
  /** A line to tell the person (a toast). */
  std::function<void(const std::string& text)> say;
  /** A call is ringing here or going on: the call stack's own state, not who holds a device. */
  std::function<bool()> call_live;
  /** The touch fingers down now (ids); empty on desktop. Read on every tick of a hold. */
  std::function<std::vector<uint64_t>()> fingers_down;
  /** Stop a playing clip and run `then` once it has let go of the speaker (at once when nothing plays). */
  std::function<void(std::function<void()> then)> stop_playback_then;
  /** Defaults: the app's UI queue, the OS microphone permission, the arbiter's microphone. */
  std::function<void(std::function<void()>)> post_ui;
  std::function<MicPermission()> mic_state;
  std::function<void(std::function<void(bool granted)>)> request_mic;
  std::function<std::unique_ptr<IVoiceCaptureSource>()> make_source;
};

/**
 * The microphone of the chat composer: hold to record and release to send (phones), or click to start and
 * Send / Cancel (desktop). The decisions are `VoiceGesture`'s, `VoiceTouchHold`'s and `VoiceRecorder`'s; this
 * class connects them to the button, the model fields and the pointer events, and hands the finished file to
 * the upload.
 *
 * The pointer events after a press are listened to on the whole document, so a finger that slid off the
 * button still ends the recording. On phones the touch state is also read on every `Tick`: a finger that is
 * gone without a release cancels the recording and never sends it. A call (from ringing on) refuses a new
 * recording and ends a running one with what was said; locking the vault or leaving the chat discards it.
 */
class VoiceComposer : public ui::EventListener {
public:
  VoiceComposer(ChatScreenState& state, VoiceComposerPorts ports,
                MediaDeviceArbiter& devices = MediaDeviceArbiter::Default());
  ~VoiceComposer() override;

  /** Recompute the button: shown, grey (a call is live), its tooltip. Cheap; call on every chrome update. */
  void Refresh();

  /** `mousedown` on the button (phones). */
  void OnPress(ui::Event& ev);
  /** `click` on the button (desktop). */
  void OnClick(ui::Event& ev);
  /** The press / click itself: `y` the pointer's vertical position, `doc` the document to follow the pointer in. */
  void BeginAt(float y, ui::ElementDocument* doc, bool hold);
  /** The pointer moved / was released while a press is followed (what the document listener forwards). */
  void OnMove(float y);
  void OnRelease();
  /** The Send / Cancel buttons of a desktop recording. */
  void OnSendButton();
  void OnCancelButton();
  /** Esc: cancels a recording. True when there was one. */
  bool OnEscape();
  /** The press can no longer be followed (chat changed, app went to the background): cancel what is being held. */
  void Lost();
  /** Everything goes: a recording running or just finishing is thrown away and nothing is sent (vault locked, chat left). */
  void DiscardAll();
  /** Every frame while a recording or its press is on: the touch hold, a call turning live. */
  void Tick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  /** A recording is on, or being started or ended. */
  bool Active() const;
  bool Recording() const { return gesture_.Recording(); }
  void Shutdown();

  void ProcessEvent(ui::Event& event) override;

private:
  void StartRecording();
  void LaunchRecorder(int generation, const VoiceRecorderConfig& config);
  void Apply(VoiceGesture::Action action);
  void OnProgress(int generation, int64_t elapsed_ms);
  void OnFinished(int generation, VoiceRecording recording);
  void EndWithoutRecorder();
  void CancelRecorder();
  void ShowRecording(bool on);
  void SetHint();
  void AttachDocument(ui::ElementDocument* doc);
  void DetachDocument();
  void Say(const std::string& text) const;
  bool HoldFingerGone() const;

  ChatScreenState& state_;
  VoiceComposerPorts ports_;
  MediaDeviceArbiter& devices_;
  VoiceGesture gesture_;
  VoiceTouchHold touch_;
  std::unique_ptr<VoiceRecorder> recorder_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  ui::ElementDocument* document_ = nullptr;
  int generation_ = 0;
  int max_seconds_ = 120;
  bool pointer_down_ = false;
  /** This recording is a hold (a phone's press), not a desktop click. */
  bool hold_mode_ = false;
  bool user_ended_ = false;
  bool user_cancelled_ = false;
  /** The recorder is still waiting for a playing clip to let go of the speaker. */
  bool start_pending_ = false;
  bool in_flight_ = false;
  int64_t elapsed_ms_ = 0;
  std::string thread_id_;
  std::chrono::steady_clock::time_point last_call_check_{};
  bool shut_down_ = false;
};

} // namespace pbr
