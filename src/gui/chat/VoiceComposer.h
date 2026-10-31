#pragma once

#include "gui/chat/ChatScreenState.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/VoiceGesture.h"
#include "lib/media/VoiceRecorder.h"
#include "lib/messaging/AttachmentLimits.h"

#include <ui/dom/EventListener.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

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
  /** A finished recording to send as an attachment; the file is deleted once it has been read. */
  std::function<void(const std::string& path)> send;
  /** This chat can take a voice message (a peer or group chat, messaging ready, nothing else being sent). */
  std::function<bool()> eligible;
  /** The chat model changed. */
  std::function<void()> dirty;
  /** A line to tell the person (a toast). */
  std::function<void(const std::string& text)> say;
};

/**
 * The microphone of the chat composer: hold to record and release to send (phones), or click to start and
 * Send / Cancel (desktop). The decisions are `VoiceGesture`'s and `VoiceRecorder`'s; this class connects
 * them to the button, the model fields and the pointer events, and hands the finished file to the upload.
 *
 * The pointer events after a press are listened to on the whole document, so a finger that slid off the
 * button still ends the recording; one that is never heard from again is `Lost()` and cancels.
 */
class VoiceComposer : public ui::EventListener {
public:
  VoiceComposer(ChatScreenState& state, VoiceComposerPorts ports,
                MediaDeviceArbiter& devices = MediaDeviceArbiter::Default());
  ~VoiceComposer() override;

  /** Recompute the button: shown, grey (a call has the microphone), its tooltip. Cheap; call on every chrome update. */
  void Refresh();

  /** `mousedown` on the button (phones). */
  void OnPress(ui::Event& ev);
  /** `click` on the button (desktop). */
  void OnClick(ui::Event& ev);
  /** The Send / Cancel buttons of a desktop recording. */
  void OnSendButton();
  void OnCancelButton();
  /** Esc: cancels a recording. True when there was one. */
  bool OnEscape();
  /** The press can no longer be followed (chat changed, app went to the background, window lost): cancel. */
  void Lost();
  bool Recording() const { return gesture_.Recording(); }
  void Shutdown();

  void ProcessEvent(ui::Event& event) override;

private:
  void Begin(ui::Event& ev, bool hold);
  void StartRecording();
  void Apply(VoiceGesture::Action action);
  void OnProgress(int generation, int64_t elapsed_ms);
  void OnFinished(int generation, VoiceRecording recording);
  void ShowRecording(bool on);
  void SetHint();
  void AttachDocument(ui::ElementDocument* doc);
  void DetachDocument();
  void Say(const std::string& text) const;

  ChatScreenState& state_;
  VoiceComposerPorts ports_;
  MediaDeviceArbiter& devices_;
  VoiceGesture gesture_;
  std::unique_ptr<VoiceRecorder> recorder_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
  ui::ElementDocument* document_ = nullptr;
  int generation_ = 0;
  int max_seconds_ = 120;
  bool pointer_down_ = false;
  bool user_ended_ = false;
  bool user_cancelled_ = false;
  int64_t elapsed_ms_ = 0;
  bool shut_down_ = false;
};

} // namespace pbr
