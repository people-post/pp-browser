#include "gui/chat/VoiceComposer.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/Platform.h"
#include "common/runtime/AppRuntime.h"
#include "lib/voice/VoiceSupport.h"

#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace pbr {
namespace {

/** The last seconds of a recording show a count down. */
constexpr int64_t kCountdownMs = 10 * 1000;
/** A hold shorter than this is a tap on the bar, not a recording. */
constexpr std::chrono::milliseconds kTapMax{250};
/** The level bars' shortest and tallest, dp. */
constexpr float kBarMinDp = 4.f;
constexpr float kBarMaxDp = 20.f;
/** How often the call stack is asked whether a call is live while a recording is on. */
constexpr std::chrono::milliseconds kCallCheckEvery{250};

std::string Dp(const float value) {
  return std::to_string(static_cast<int>(std::lround(value))) + "dp";
}

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
  for (ui::String& bar : state_.voice_lv) {
    bar = Dp(kBarMinDp).c_str();
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
  // No microphone here (another chat, a call setup, a send in progress): back to the text input.
  const bool leave_voice_mode = !show && state_.voice_mode;
  if (state_.show_voice == show && state_.voice_blocked == blocked && state_.voice_click_mode == click_mode &&
      std::string(state_.voice_title) == title && !leave_voice_mode) {
    return;
  }
  if (leave_voice_mode) {
    state_.voice_mode = false;
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
  BeginAt(static_cast<float>(ev.GetParameter<int>("mouse_x", 0)),
          static_cast<float>(ev.GetParameter<int>("mouse_y", 0)), target ? target->GetOwnerDocument() : nullptr,
          /*hold=*/true);
}

void VoiceComposer::OnClick(ui::Event& ev) {
  if (Platform::IsMobile()) {
    return; // a phone started on the press
  }
  BeginAt(static_cast<float>(ev.GetParameter<int>("mouse_x", 0)),
          static_cast<float>(ev.GetParameter<int>("mouse_y", 0)), nullptr, /*hold=*/false);
}

void VoiceComposer::BeginAt(const float x_px, const float y_px, ui::ElementDocument* doc, const bool hold) {
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
  // The pointer comes in pixels of the window; every distance below is in dp.
  const float ratio = ports_.dp_ratio && ports_.dp_ratio() > 0.f ? ports_.dp_ratio() : 1.f;
  const float x = x_px / ratio;
  const float y = y_px / ratio;
  gesture_ = VoiceGesture(hold ? VoiceGesture::Mode::Hold : VoiceGesture::Mode::Click);
  if (gesture_.Press(x, y) != VoiceGesture::Action::Start) {
    return;
  }
  thread_id_ = thread_id;
  hold_mode_ = hold;
  press_time_ = std::chrono::steady_clock::now();
  press_y_dp_ = y;
  shown_left_ = shown_top_ = shown_tail_ = -1000.f;
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
  call_discarded_ = false;
  DropHeldRecording();
  std::fill(std::begin(levels_), std::end(levels_), 0.f);
  elapsed_ms_ = 0;
  const AttachmentLimits limits = ports_.limits ? ports_.limits() : AttachmentLimits{};
  max_seconds_ = limits.voice_max_seconds;

  VoiceRecorderConfig config;
  config.max_seconds = max_seconds_;
  config.out_dir = ports_.out_dir();
  std::error_code ec;
  // Nothing from an earlier recording is left to be sent by mistake; a failed one that waits for Retry stays.
  const std::string keep = ports_.keep_file ? ports_.keep_file() : std::string();
  if (keep.empty()) {
    std::filesystem::remove_all(config.out_dir, ec);
  } else {
    const std::filesystem::path keep_path = std::filesystem::path(keep).lexically_normal();
    for (const auto& entry : std::filesystem::directory_iterator(config.out_dir, ec)) {
      if (entry.path().lexically_normal() != keep_path) {
        std::filesystem::remove_all(entry.path(), ec);
      }
    }
  }

  const auto alive = alive_;
  const int generation = generation_;
  VoiceRecorder::Callbacks callbacks;
  // The recorder's thread may outlive this object: its callbacks hold copies, and `alive` guards `this`.
  const auto post = ports_.post_ui;
  callbacks.on_progress = [this, alive, generation, post](const int64_t elapsed_ms, const float level) {
    post([this, alive, generation, elapsed_ms, level]() {
      if (alive->load()) {
        OnProgress(generation, elapsed_ms, level);
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
  ShowFinishing(false);
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
    ShowFinishing(true); // the encoder is still writing the file: say so until the upload takes over
  } else if (action == VoiceGesture::Action::Cancel) {
    user_ended_ = true;
    user_cancelled_ = true;
    recorder_->Cancel();
    ShowRecording(false);
    ShowFinishing(false);
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
  const bool held = limit_held_;
  Apply(gesture_.Lost()); // cancels a recording in progress; never sends
  if (held) {
    DropHeldRecording(); // it had stopped at its limit: only its file is left
    ShowRecording(false);
  }
}

void VoiceComposer::DropHeldRecording() {
  if (!limit_held_) {
    return;
  }
  RemoveQuietly(held_.path);
  held_ = VoiceRecording();
  limit_held_ = false;
  state_.voice_limit = false;
}

void VoiceComposer::DiscardForCall() {
  if (call_discarded_) {
    return; // the other path (the clock tick, the recorder's own end) got here first
  }
  call_discarded_ = true;
  pointer_down_ = false;
  DetachDocument();
  gesture_.Lost();
  user_ended_ = true;
  user_cancelled_ = true; // its result, when it still arrives, is deleted
  CancelRecorder();
  DropHeldRecording();
  ShowRecording(false);
  ShowFinishing(false);
  Say(Tr("chat.voice.interrupted_by_call"));
}

void VoiceComposer::HoldAtLimit(VoiceRecording recording) {
  held_ = std::move(recording);
  limit_held_ = true;
  gesture_.RecorderLimit(/*pointer_down=*/true);
  state_.voice_limit = true;
  state_.voice_countdown = false;
  SetHint();
  UpdateBubble();
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
  if (limit_held_) {
    DropHeldRecording();
    ShowRecording(false);
  }
  ShowFinishing(false);
}

bool VoiceComposer::HoldFingerGone() const {
  return touch_.FollowsFinger() && ports_.fingers_down && !touch_.StillDown(ports_.fingers_down());
}

void VoiceComposer::Tick(const std::chrono::steady_clock::time_point now) {
  if (!Active()) {
    return;
  }
  // A call turning live (ringing counts) throws away what is being said: nothing is sent without the person's act.
  if (((in_flight_ && !user_ended_) || limit_held_) && ports_.call_live && now - last_call_check_ >= kCallCheckEvery) {
    last_call_check_ = now;
    if (ports_.call_live()) {
      DiscardForCall();
      return;
    }
  }
  // Phones: the finger that pressed must still be down, or the press was lost and nothing is sent.
  if (hold_mode_ && pointer_down_ && ports_.fingers_down &&
      (gesture_.Recording() || gesture_.AtLimit() || gesture_.phase() == VoiceGesture::Phase::Swallowing)) {
    if (touch_.Tick(ports_.fingers_down()) == VoiceTouchHold::Verdict::Lost) {
      Lost();
    }
  }
}

void VoiceComposer::OnMove(const float x_px, const float y_px) {
  if (gesture_.Recording() || gesture_.AtLimit()) {
    const float ratio = ports_.dp_ratio && ports_.dp_ratio() > 0.f ? ports_.dp_ratio() : 1.f;
    gesture_.Move(x_px / ratio, y_px / ratio);
    if (state_.voice_cancel_zone != gesture_.InCancelZone()) {
      state_.voice_cancel_zone = gesture_.InCancelZone();
      SetHint();
    }
    UpdateBubble();
  }
}

void VoiceComposer::OnRelease(const std::chrono::steady_clock::time_point now) {
  pointer_down_ = false;
  DetachDocument();
  if (limit_held_) {
    // The recording stopped at its limit and waited for this: the release sends, an armed cancel deletes.
    const bool send = gesture_.Release() == VoiceGesture::Action::Send;
    VoiceRecording recording = std::move(held_);
    held_ = VoiceRecording();
    limit_held_ = false;
    user_ended_ = true;
    ShowRecording(false);
    if (send && !recording.path.empty() && ports_.send) {
      ports_.send(thread_id_, recording.path);
    } else {
      RemoveQuietly(recording.path);
    }
    Refresh();
    return;
  }
  VoiceGesture::Action action = gesture_.Release();
  if (action == VoiceGesture::Action::Send && hold_mode_ && now - press_time_ < kTapMax) {
    // A tap on the bar, not a message: nothing is sent and no file is left; it says how to use the bar.
    Apply(VoiceGesture::Action::Cancel);
    Say(Tr("chat.voice.tap_hint"));
    return;
  }
  Apply(action);
}

void VoiceComposer::ProcessEvent(ui::Event& event) {
  switch (event.GetId()) {
    case ui::EventId::Mousemove:
      OnMove(static_cast<float>(event.GetParameter<int>("mouse_x", 0)),
             static_cast<float>(event.GetParameter<int>("mouse_y", 0)));
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
  state_.voice_bubble = on && hold_mode_;
  if (on) {
    state_.voice_timer = ElapsedText(0).c_str();
    for (ui::String& bar : state_.voice_lv) {
      bar = Dp(kBarMinDp).c_str();
    }
    UpdateBubble();
  } else {
    state_.voice_cancel_zone = false;
    state_.voice_countdown = false;
    state_.voice_limit = false;
  }
  SetHint();
}

void VoiceComposer::DirtyBubble() const {
  if (ports_.dirty_bubble) {
    ports_.dirty_bubble();
  } else if (ports_.dirty) {
    ports_.dirty();
  }
}

void VoiceComposer::UpdateBubble() {
  if (!hold_mode_) {
    return;
  }
  const float width = ports_.viewport_width_dp ? ports_.viewport_width_dp() : 393.f;
  const float anchor = gesture_.anchor_x();
  const float left = VoiceBubbleLeft(anchor, width);
  const float tail = VoiceBubbleTail(anchor, left);
  // The bubble's bottom edge sits above the cancel line; it never rises into the header.
  const float top_limit = ports_.top_limit_dp ? ports_.top_limit_dp() : 0.f;
  const float top = std::max(top_limit, press_y_dp_ - voice_dp::kBubbleLift - voice_dp::kBubbleHeight);
  if (std::fabs(left - shown_left_) < 1.f && std::fabs(top - shown_top_) < 1.f && std::fabs(tail - shown_tail_) < 1.f) {
    return;
  }
  shown_left_ = left;
  shown_top_ = top;
  shown_tail_ = tail;
  state_.voice_bubble_left = Dp(left).c_str();
  state_.voice_bubble_top = Dp(top).c_str();
  state_.voice_tail_left = Dp(tail).c_str();
  DirtyBubble();
}

void VoiceComposer::ShowFinishing(const bool on) {
  if (state_.voice_finishing == on) {
    return;
  }
  state_.voice_finishing = on;
  if (ports_.dirty) {
    ports_.dirty();
  }
}

void VoiceComposer::SetHint() {
  std::string hint;
  if (state_.voice_recording) {
    const int64_t left_ms = static_cast<int64_t>(max_seconds_) * 1000 - elapsed_ms_;
    const std::string countdown =
        Tr("chat.voice.countdown", {{"seconds", std::to_string(std::max<int64_t>(1, (left_ms + 999) / 1000))}});
    if (state_.voice_limit) {
      hint = Tr("chat.voice.limit_reached", {{"minutes", std::to_string(std::max(1, max_seconds_ / 60))}});
    } else if (!hold_mode_) {
      hint = left_ms <= kCountdownMs ? countdown : Tr("chat.voice.hint_click"); // desktop: the bar's own hint
    } else if (left_ms <= kCountdownMs && !state_.voice_cancel_zone) {
      hint = countdown; // phones: the bubble's first line
    }
  }
  state_.voice_hint = hint.c_str();
  if (ports_.dirty) {
    ports_.dirty();
  }
}

void VoiceComposer::OnProgress(const int generation, const int64_t elapsed_ms, const float level) {
  if (generation != generation_ || !in_flight_ || !state_.voice_recording) {
    return;
  }
  elapsed_ms_ = elapsed_ms;
  if (!hold_mode_ && !user_ended_ && elapsed_ms >= static_cast<int64_t>(max_seconds_) * 1000) {
    ShowFinishing(true); // the desktop's recorder ends itself at the limit and sends: "Sending…"
  }
  if (hold_mode_) {
    // The newest level enters on the right; the bars are written here, on the UI thread, never by the recorder.
    std::rotate(std::begin(levels_), std::begin(levels_) + 1, std::end(levels_));
    levels_[ChatScreenState::kVoiceLevelBars - 1] = std::clamp(level, 0.f, 1.f);
    for (int i = 0; i < ChatScreenState::kVoiceLevelBars; ++i) {
      state_.voice_lv[i] = Dp(kBarMinDp + levels_[i] * (kBarMaxDp - kBarMinDp)).c_str();
    }
    DirtyBubble();
  }
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
  ShowFinishing(false);
  if (recording.end == VoiceEnd::Interrupted && !user_ended_) {
    DiscardForCall(); // the microphone was taken (a call): the same discard as the clock tick's, said once
  }
  if (recording.end == VoiceEnd::LimitReached && !user_ended_ && !call_discarded_ && hold_mode_ &&
      gesture_.Recording()) {
    // The limit under a finger: stop here and let the release decide (send, or slide up to cancel).
    if (HoldFingerGone()) {
      RemoveQuietly(recording.path); // lifted before the limit with no release seen: a lost press
      gesture_.Lost();
      DetachDocument();
      ShowRecording(false);
      Refresh();
      return;
    }
    HoldAtLimit(std::move(recording));
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
    case VoiceEnd::LimitReached: // the desktop's: it sends at the limit
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
