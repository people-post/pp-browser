#include "gui/chat/VoiceComposer.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/Platform.h"
#include "foundation/runtime/AppRuntime.h"
#include "lib/media/MicPermission.h"
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
    : state_(state), ports_(std::move(ports)), devices_(devices) {}

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
  if (recorder_) {
    recorder_->Cancel();
    recorder_.reset();
  }
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

void VoiceComposer::Refresh() {
  const bool show = VoiceMessagesSupported() && ports_.eligible && ports_.eligible();
  const bool blocked = show && !VoiceDeviceBlockedBy(devices_).empty();
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
  Begin(ev, /*hold=*/true);
}

void VoiceComposer::OnClick(ui::Event& ev) {
  if (Platform::IsMobile()) {
    return; // a phone started on the press
  }
  Begin(ev, /*hold=*/false);
}

void VoiceComposer::Begin(ui::Event& ev, const bool hold) {
  Refresh();
  if (!state_.show_voice) {
    return;
  }
  if (gesture_.phase() == VoiceGesture::Phase::Swallowing) {
    gesture_.Lost(); // a new press means the one we were swallowing is over
  }
  if (gesture_.phase() != VoiceGesture::Phase::Idle || (recorder_ && recorder_->IsRecording())) {
    return;
  }
  if (state_.voice_blocked) {
    Say(Tr("chat.voice.busy_call"));
    return;
  }
  if (MicPermissionState() != MicPermission::Granted) {
    // The first press only asks. (A refusal made earlier answers at once, so the person is told what to change.)
    const auto alive = alive_;
    RequestMicPermission([this, alive, hold](const bool granted) {
      AppRuntime::PostUI([this, alive, hold, granted]() {
        if (!alive->load()) {
          return;
        }
        Say(Tr(granted ? (hold ? "chat.voice.try_again_hold" : "chat.voice.try_again_click")
                       : "chat.voice.permission_denied"));
      });
    });
    return;
  }

  gesture_ = VoiceGesture(hold ? VoiceGesture::Mode::Hold : VoiceGesture::Mode::Click);
  const float y = static_cast<float>(ev.GetParameter<int>("mouse_y", 0));
  if (gesture_.Press(y) != VoiceGesture::Action::Start) {
    return;
  }
  pointer_down_ = hold;
  StartRecording();
  if (hold) {
    if (ui::Element* target = ev.GetTargetElement()) {
      AttachDocument(target->GetOwnerDocument());
    }
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
  config.out_dir = ports_.out_dir ? ports_.out_dir() : std::string();
  if (!config.out_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(config.out_dir, ec); // nothing from an earlier recording is left to be sent by mistake
  }

  const auto alive = alive_;
  const int generation = generation_;
  VoiceRecorder::Callbacks callbacks;
  callbacks.on_progress = [this, alive, generation](const int64_t elapsed_ms) {
    AppRuntime::PostUI([this, alive, generation, elapsed_ms]() {
      if (alive->load()) {
        OnProgress(generation, elapsed_ms);
      }
    });
  };
  callbacks.on_finished = [this, alive, generation](VoiceRecording recording) {
    AppRuntime::PostUI([this, alive, generation, recording = std::move(recording)]() mutable {
      if (alive->load()) {
        OnFinished(generation, std::move(recording));
      } else {
        RemoveQuietly(recording.path);
      }
    });
  };
  // A new recorder per recording: results of the one before stay with their own generation.
  recorder_ = std::make_unique<VoiceRecorder>([this]() { return CreateArbiterVoiceCaptureSource(devices_); },
                                              std::move(callbacks));
  ShowRecording(true);
  recorder_->Start(config);
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

void VoiceComposer::ProcessEvent(ui::Event& event) {
  switch (event.GetId()) {
  case ui::EventId::Mousemove:
    if (gesture_.Recording()) {
      gesture_.Move(static_cast<float>(event.GetParameter<int>("mouse_y", 0)));
      if (state_.voice_cancel_zone != gesture_.InCancelZone()) {
        state_.voice_cancel_zone = gesture_.InCancelZone();
        SetHint();
      }
    }
    break;
  case ui::EventId::Mouseup:
    pointer_down_ = false;
    DetachDocument();
    Apply(gesture_.Release());
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
  const bool self_ended = !user_ended_;
  if (self_ended) {
    // The limit, a call, or a failure ended it while a finger may still be down: swallow the rest of that press.
    gesture_.RecorderEnded(pointer_down_);
    if (!pointer_down_) {
      DetachDocument();
    }
  }
  ShowRecording(false);

  switch (recording.end) {
  case VoiceEnd::Sent:
  case VoiceEnd::LimitReached:
  case VoiceEnd::Interrupted:
    if (user_cancelled_ || recording.path.empty()) {
      RemoveQuietly(recording.path);
    } else if (ports_.send) {
      ports_.send(recording.path);
    }
    break;
  case VoiceEnd::TooShort:
    if (!user_cancelled_) {
      Say(Tr("chat.voice.too_short"));
    }
    break;
  case VoiceEnd::Cancelled:
    RemoveQuietly(recording.path);
    break;
  case VoiceEnd::Busy:
    Say(Tr("chat.voice.busy_call"));
    break;
  case VoiceEnd::NoDevice:
    Say(Tr(MicPermissionState() == MicPermission::Denied ? "chat.voice.permission_denied" : "chat.voice.no_mic"));
    break;
  case VoiceEnd::Failed:
    Say(Tr("chat.voice.failed"));
    break;
  }
  Refresh();
}

} // namespace pbr
