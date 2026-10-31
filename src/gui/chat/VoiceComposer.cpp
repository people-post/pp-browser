#include "gui/chat/VoiceComposer.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/Platform.h"
#include "common/runtime/AppRuntime.h"
#include "lib/media/VoiceSupport.h"

#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

#include <algorithm>
#include <filesystem>

namespace pbr {
namespace {

/** The last seconds of a recording show a count down. */
constexpr int64_t kCountdownMs = 10 * 1000;
/** How often the call stack is asked whether a call is live while a recording is on. */
constexpr std::chrono::milliseconds kCallCheckEvery{250};

std::string ElapsedText(const int64_t elapsed_ms) {
  const int64_t seconds = elapsed_ms / 1000;
  return std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}

void RemoveQuietly(const std::string& path) {
  if (!path.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

} // namespace

VoiceComposer::VoiceComposer(ChatScreenState& state, VoiceComposerPorts ports, MediaDeviceArbiter& devices)
    : state_(state), ports_(std::move(ports)), devices_(devices) {
  if (!ports_.post_ui) {
    ports_.post_ui = [](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); };
  }
  if (!ports_.mic_state) {
    ports_.mic_state = []() { return MicPermissionState(); };
  }
  if (!ports_.request_mic) {
    ports_.request_mic = [](std::function<void(bool)> done) { RequestMicPermission(std::move(done)); };
  }
  if (!ports_.make_source) {
    MediaDeviceArbiter* devices_ptr = &devices_;
    ports_.make_source = [devices_ptr]() { return CreateArbiterVoiceCaptureSource(*devices_ptr); };
  }
}

VoiceComposer::~VoiceComposer() {
  Shutdown();
}

void VoiceComposer::Shutdown() {
  if (shut_down_) {
    return;
  }
  shut_down_ = true;
  alive_->store(false);
  DetachDocument();
  CancelRecorder();
  recorder_.reset();
  gesture_ = VoiceGesture();
  if (ports_.out_dir) {
    std::error_code ec;
    std::filesystem::remove_all(ports_.out_dir(), ec);
  }
}

void VoiceComposer::Say(const std::string& text) const {
  if (ports_.say) {
    ports_.say(text);
  }
}

bool VoiceComposer::Active() const {
  return in_flight_ || gesture_.phase() != VoiceGesture::Phase::Idle;
}

void VoiceComposer::Refresh() {
  const bool show = VoiceMessagesSupported() && ports_.eligible && ports_.eligible();
  const bool blocked = show && ports_.call_live && ports_.call_live();
  const bool click_mode = !Platform::IsMobile();
  const std::string title = Tr(blocked ? "chat.voice.title_call" : "chat.voice.title");
  if (state_.show_voice == show && state_.voice_blocked == blocked && state_.voice_click_mode == click_mode &&
      std::string(state_.voice_title) == title) {
    return;
  }
  state_.show_voice = show;
  state_.voice_blocked = blocked;
  state_.voice_click_mode = click_mode;
  state_.voice_title = title.c_str();
  if (ports_.dirty) {
    ports_.dirty();
  }
}

void VoiceComposer::OnPress(ui::Event& ev) {
  if (!Platform::IsMobile()) {
    return; // desktop starts on click
  }
  ui::Element* target = ev.GetTargetElement();
  BeginAt(static_cast<float>(ev.GetParameter<int>("mouse_y", 0)), target ? target->GetOwnerDocument() : nullptr,
          /*hold=*/true);
}

void VoiceComposer::OnClick(ui::Event& ev) {
  if (Platform::IsMobile()) {
    return; // a phone started on the press
  }
  BeginAt(static_cast<float>(ev.GetParameter<int>("mouse_y", 0)), nullptr, /*hold=*/false);
}

void VoiceComposer::BeginAt(const float y, ui::ElementDocument* doc, const bool hold) {
  Refresh();
  if (!state_.show_voice) {
    return;
  }
  if (gesture_.phase() == VoiceGesture::Phase::Swallowing) {
    gesture_.Lost(); // a new press means the one we were swallowing is over
  }
  if (Active() || (recorder_ && recorder_->IsRecording())) {
    return;
  }
  if (state_.voice_blocked) {
    Say(Tr("chat.voice.busy_call"));
    return;
  }
  if (ports_.mic_state() != MicPermission::Granted) {
    // The first press only asks. (A refusal made earlier answers at once, so the person is told what to change.)
    const auto alive = alive_;
    ports_.request_mic([this, alive, hold](const bool granted) {
      ports_.post_ui([this, alive, hold, granted]() {
        if (!alive->load()) {
          return;
        }
        Say(Tr(granted ? (hold ? "chat.voice.try_again_hold" : "chat.voice.try_again_click")
                       : "chat.voice.permission_denied"));
      });
    });
    return;
  }
  // The chat is read once, here: the message goes to the chat the person pressed in, whatever is open later.
  const std::string thread_id = ports_.thread_id ? ports_.thread_id() : std::string();
  if (thread_id.empty() || !ports_.out_dir || ports_.out_dir().empty()) {
    Say(Tr("chat.voice.failed"));
    return;
  }
  gesture_ = VoiceGesture(hold ? VoiceGesture::Mode::Hold : VoiceGesture::Mode::Click);
  if (gesture_.Press(y) != VoiceGesture::Action::Start) {
    return;
  }
  thread_id_ = thread_id;
  hold_mode_ = hold;
  pointer_down_ = hold;
  touch_ = VoiceTouchHold();
  if (hold) {
    touch_.Press(ports_.fingers_down ? ports_.fingers_down() : std::vector<uint64_t>{});
  }
  StartRecording();
  if (hold && doc) {
    AttachDocument(doc);
  }
}

void VoiceComposer::StartRecording() {
  ++generation_;
  user_ended_ = false;
  user_cancelled_ = false;
  elapsed_ms_ = 0;
  const AttachmentLimits limits = ports_.limits ? ports_.limits() : AttachmentLimits{};
  max_seconds_ = limits.voice_max_seconds;

  VoiceRecorderConfig config;
  config.max_seconds = max_seconds_;
  config.out_dir = ports_.out_dir();
  std::error_code ec;
  std::filesystem::remove_all(config.out_dir, ec); // nothing from an earlier recording is left to be sent by mistake

  const auto alive = alive_;
  const int generation = generation_;
  VoiceRecorder::Callbacks callbacks;
  // The recorder's thread may outlive this object: its callbacks hold copies, and `alive` guards `this`.
  const auto post = ports_.post_ui;
  callbacks.on_progress = [this, alive, generation, post](const int64_t elapsed_ms) {
    post([this, alive, generation, elapsed_ms]() {
      if (alive->load()) {
        OnProgress(generation, elapsed_ms);
      }
    });
  };
  callbacks.on_finished = [this, alive, generation, post](VoiceRecording recording) {
    post([this, alive, generation, recording = std::move(recording)]() mutable {
      if (alive->load()) {
        OnFinished(generation, std::move(recording));
      } else {
        RemoveQuietly(recording.path);
      }
    });
  };
  // A new recorder per recording: results of the one before stay with their own generation.
  recorder_ = std::make_unique<VoiceRecorder>(ports_.make_source, std::move(callbacks));
  ShowRecording(true);
  in_flight_ = true;
  start_pending_ = true;

  // A clip that is playing lets go of the speaker (and the audio session) before the microphone is opened.
  const auto go = [this, alive, generation, config]() {
    if (!alive->load() || generation != generation_) {
      return;
    }
    start_pending_ = false;
    if (user_ended_ || user_cancelled_) {
      EndWithoutRecorder(); // released or cancelled before the microphone was even asked for
      return;
    }
    recorder_->Start(config);
  };
  if (ports_.stop_playback_then) {
    ports_.stop_playback_then(go);
  } else {
    go();
  }
}

void VoiceComposer::EndWithoutRecorder() {
  in_flight_ = false;
  ShowRecording(false);
  if (!user_cancelled_) {
    Say(Tr("chat.voice.too_short"));
  }
  Refresh();
}

void VoiceComposer::CancelRecorder() {
  if (recorder_) {
    recorder_->Cancel();
  }
}

void VoiceComposer::Apply(const VoiceGesture::Action action) {
  if (!recorder_) {
    return;
  }
  if (action == VoiceGesture::Action::Send) {
    user_ended_ = true;
    recorder_->Finish();
    ShowRecording(false);
  } else if (action == VoiceGesture::Action::Cancel) {
    user_ended_ = true;
    user_cancelled_ = true;
    recorder_->Cancel();
    ShowRecording(false);
  }
}

void VoiceComposer::OnSendButton() {
  Apply(gesture_.SendNow());
}

void VoiceComposer::OnCancelButton() {
  Apply(gesture_.CancelNow());
}

bool VoiceComposer::OnEscape() {
  if (!gesture_.Recording()) {
    return false;
  }
  Apply(gesture_.CancelNow());
  DetachDocument();
  return true;
}

void VoiceComposer::Lost() {
  pointer_down_ = false;
  DetachDocument();
  Apply(gesture_.Lost()); // cancels a recording in progress; never sends
}

void VoiceComposer::DiscardAll() {
  pointer_down_ = false;
  DetachDocument();
  gesture_.Lost();
  if (in_flight_) {
    user_ended_ = true;
    user_cancelled_ = true; // even one that was released a moment ago: its result is deleted, not sent
    CancelRecorder();
    ShowRecording(false);
  }
}

bool VoiceComposer::HoldFingerGone() const {
  return touch_.FollowsFinger() && ports_.fingers_down && !touch_.StillDown(ports_.fingers_down());
}

void VoiceComposer::Tick(const std::chrono::steady_clock::time_point now) {
  if (!Active()) {
    return;
  }
  // A call turning live (ringing counts) ends what is being said: the recording keeps what was recorded.
  if (in_flight_ && !user_ended_ && ports_.call_live && now - last_call_check_ >= kCallCheckEvery) {
    last_call_check_ = now;
    if (ports_.call_live()) {
      if (start_pending_) {
        user_ended_ = true;
        user_cancelled_ = true;
      } else if (recorder_) {
        recorder_->Finish(); // not the person's own end: the rest of a held press is swallowed when it arrives
      }
    }
  }
  // Phones: the finger that pressed must still be down, or the press was lost and nothing is sent.
  if (hold_mode_ && pointer_down_ && ports_.fingers_down &&
      (gesture_.Recording() || gesture_.phase() == VoiceGesture::Phase::Swallowing)) {
    if (touch_.Tick(ports_.fingers_down()) == VoiceTouchHold::Verdict::Lost) {
      Lost();
    }
  }
}

void VoiceComposer::OnMove(const float y) {
  if (gesture_.Recording()) {
    gesture_.Move(y);
    if (state_.voice_cancel_zone != gesture_.InCancelZone()) {
      state_.voice_cancel_zone = gesture_.InCancelZone();
      SetHint();
    }
  }
}

void VoiceComposer::OnRelease() {
  pointer_down_ = false;
  DetachDocument();
  Apply(gesture_.Release());
}

void VoiceComposer::ProcessEvent(ui::Event& event) {
  switch (event.GetId()) {
  case ui::EventId::Mousemove:
    OnMove(static_cast<float>(event.GetParameter<int>("mouse_y", 0)));
    break;
  case ui::EventId::Mouseup:
    OnRelease();
    break;
  default:
    break;
  }
}

void VoiceComposer::AttachDocument(ui::ElementDocument* doc) {
  DetachDocument();
  if (!doc) {
    return;
  }
  document_ = doc;
  doc->AddEventListener(ui::EventId::Mousemove, this, true);
  doc->AddEventListener(ui::EventId::Mouseup, this, true);
}

void VoiceComposer::DetachDocument() {
  if (!document_) {
    return;
  }
  document_->RemoveEventListener(ui::EventId::Mousemove, this, true);
  document_->RemoveEventListener(ui::EventId::Mouseup, this, true);
  document_ = nullptr;
}

void VoiceComposer::ShowRecording(const bool on) {
  state_.voice_recording = on;
  if (on) {
    state_.voice_timer = ElapsedText(0).c_str();
  } else {
    state_.voice_cancel_zone = false;
    state_.voice_countdown = false;
  }
  SetHint();
}

void VoiceComposer::SetHint() {
  std::string hint;
  if (state_.voice_recording) {
    const int64_t left_ms = static_cast<int64_t>(max_seconds_) * 1000 - elapsed_ms_;
    if (state_.voice_cancel_zone) {
      hint = Tr("chat.voice.hint_cancel");
    } else if (left_ms <= kCountdownMs) {
      hint = Tr("chat.voice.countdown", {{"seconds", std::to_string(std::max<int64_t>(1, (left_ms + 999) / 1000))}});
    } else {
      hint = Tr(state_.voice_click_mode ? "chat.voice.hint_click" : "chat.voice.hint_hold");
    }
  }
  state_.voice_hint = hint.c_str();
  if (ports_.dirty) {
    ports_.dirty();
  }
}

void VoiceComposer::OnProgress(const int generation, const int64_t elapsed_ms) {
  if (generation != generation_ || !state_.voice_recording) {
    return;
  }
  elapsed_ms_ = elapsed_ms;
  state_.voice_timer = ElapsedText(elapsed_ms).c_str();
  state_.voice_countdown = static_cast<int64_t>(max_seconds_) * 1000 - elapsed_ms <= kCountdownMs;
  SetHint();
}

void VoiceComposer::OnFinished(const int generation, VoiceRecording recording) {
  if (generation != generation_) {
    RemoveQuietly(recording.path); // an older recording's result: not ours to send
    return;
  }
  in_flight_ = false;
  const bool self_ended = !user_ended_;
  if (self_ended) {
    // The limit, a call, or a failure ended it while a finger may still be down: swallow the rest of that press.
    gesture_.RecorderEnded(pointer_down_);
    if (!pointer_down_) {
      DetachDocument();
    }
  }
  ShowRecording(false);

  const VoiceEnd end = recording.end;
  // What the person did comes first. A cancel says nothing (and deletes what was made); a release before the
  // microphone was ready is "too short", not a device error.
  if (user_cancelled_) {
    RemoveQuietly(recording.path);
    Refresh();
    return;
  }
  if (user_ended_ && (end == VoiceEnd::Busy || end == VoiceEnd::NoDevice || end == VoiceEnd::Failed)) {
    Say(Tr("chat.voice.too_short"));
    Refresh();
    return;
  }
  switch (end) {
  case VoiceEnd::LimitReached:
    if (hold_mode_ && HoldFingerGone()) {
      RemoveQuietly(recording.path); // the finger was lifted before the limit: that is a lost press, not a send
      break;
    }
    [[fallthrough]];
  case VoiceEnd::Sent:
  case VoiceEnd::Interrupted:
    if (recording.path.empty()) {
      break;
    }
    if (ports_.send) {
      ports_.send(thread_id_, recording.path);
    } else {
      RemoveQuietly(recording.path);
    }
    break;
  case VoiceEnd::TooShort:
    Say(Tr("chat.voice.too_short"));
    break;
  case VoiceEnd::Cancelled:
    RemoveQuietly(recording.path);
    break;
  case VoiceEnd::Busy:
    Say(Tr("chat.voice.busy_call"));
    break;
  case VoiceEnd::NoDevice:
    Say(Tr(ports_.mic_state() == MicPermission::Denied ? "chat.voice.permission_denied" : "chat.voice.no_mic"));
    break;
  case VoiceEnd::Failed:
    Say(Tr("chat.voice.failed"));
    break;
  }
  Refresh();
}

} // namespace pbr
