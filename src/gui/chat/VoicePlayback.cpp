#include "gui/chat/VoicePlayback.h"

#include "foundation/i18n/LocalizationService.h"
#include "common/runtime/AppRuntime.h"
#include "gui/chat/VoiceBubble.h"
#include "lib/media/VoiceFile.h"

#include <ui/dom/Element.h>

namespace pbr {

VoicePlayback::VoicePlayback(VoicePlaybackPorts ports, MediaDeviceArbiter& devices)
    : ports_(std::move(ports)), devices_(devices) {
  if (!ports_.post_ui) {
    ports_.post_ui = [](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); };
  }
  const auto alive = alive_;
  VoicePlayerCallbacks callbacks;
  callbacks.on_progress = [this, alive](const std::string& id, const uint64_t serial, const int64_t position_ms,
                                        const int64_t duration_ms) {
    ports_.post_ui([this, alive, id, serial, position_ms, duration_ms]() {
      if (alive->load()) {
        OnProgress(id, serial, position_ms, duration_ms);
      }
    });
  };
  callbacks.on_ended = [this, alive](const std::string& id, const uint64_t serial, const VoicePlayEnd end) {
    ports_.post_ui([this, alive, id, serial, end]() {
      if (alive->load()) {
        OnEnded(id, serial, end);
      }
    });
  };
  player_ = std::make_unique<VoicePlayer>([this]() { return CreateArbiterVoicePlaybackSink(devices_); },
                                          std::move(callbacks));
}

VoicePlayback::~VoicePlayback() {
  Shutdown();
}

void VoicePlayback::Shutdown() {
  alive_->store(false);
  player_.reset();
}

void VoicePlayback::Say(const std::string& text) const {
  if (ports_.say) {
    ports_.say(text);
  }
}

VoiceBubbleState VoicePlayback::State(const std::string& message_id) const {
  VoiceBubbleState state;
  if (message_id.empty() || message_id != current_) {
    return state;
  }
  state.playing = !paused_;
  state.paused = paused_;
  state.position_ms = position_ms_;
  state.duration_ms = duration_ms_;
  return state;
}

void VoicePlayback::Toggle(const std::string& message_id) {
  if (message_id.empty() || !player_ || message_id == resolving_) {
    return;
  }
  if (message_id == current_) {
    if (paused_) {
      player_->Resume();
      paused_ = false;
    } else {
      player_->Pause();
      paused_ = true;
    }
    Update(message_id);
    return;
  }
  if (ports_.call_live && ports_.call_live()) {
    Say(Tr("chat.voice.play_in_call")); // not during a call
    return;
  }
  if (!ports_.resolve) {
    return;
  }
  resolving_ = message_id;
  const uint64_t generation = ++resolve_generation_;
  const auto alive = alive_;
  ports_.resolve(message_id, [this, alive, message_id, generation](std::optional<std::string> path) {
    if (!alive->load()) {
      return;
    }
    // A Stop (or another tap) in the meantime cancelled this one: its file is not played late.
    if (generation != resolve_generation_ || resolving_ != message_id) {
      return;
    }
    resolving_.clear();
    if (!path) {
      Say(Tr("chat.voice.cannot_play"));
      return;
    }
    Start(message_id, *path);
  });
}

void VoicePlayback::Start(const std::string& message_id, const std::string& path) {
  const std::string before = current_;
  const std::string before_length = duration_ms_ > 0 ? FormatVoiceDuration(duration_ms_) : std::string();
  current_ = message_id;
  stopped_by_user_ = false;
  paused_ = false;
  position_ms_ = 0;
  duration_ms_ = 0;
  if (!before.empty() && before != message_id) {
    Update(before, before_length); // the clip it replaces goes back to idle
  }
  Update(message_id);
  serial_ = player_->Play(message_id, path, ports_.max_seconds ? ports_.max_seconds() : 120);
}

void VoicePlayback::Stop() {
  resolving_.clear();
  ++resolve_generation_;
  if (player_ && !current_.empty()) {
    stopped_by_user_ = true;
    player_->Stop();
  }
}

void VoicePlayback::StopThen(std::function<void()> then) {
  const bool playing = !current_.empty();
  Stop();
  if (!playing) {
    if (then) {
      then();
    }
    return;
  }
  after_stop_.push_back(std::move(then)); // runs when the clip has ended: its speaker is closed by then
}

void VoicePlayback::OnCallLive() {
  Stop();
}

void VoicePlayback::OnProgress(const std::string& message_id, const uint64_t serial, const int64_t position_ms,
                               const int64_t duration_ms) {
  if (message_id != current_ || serial != serial_) {
    return;
  }
  position_ms_ = position_ms;
  duration_ms_ = duration_ms;
  Update(message_id);
}

void VoicePlayback::OnEnded(const std::string& message_id, const uint64_t serial, const VoicePlayEnd end) {
  if (message_id != current_ || serial != serial_) {
    return; // a clip that was replaced: its bubble was already reset
  }
  const std::string length = duration_ms_ > 0 ? FormatVoiceDuration(duration_ms_) : std::string();
  const bool by_user = stopped_by_user_;
  current_.clear();
  serial_ = 0;
  stopped_by_user_ = false;
  paused_ = false;
  position_ms_ = 0;
  duration_ms_ = 0;
  Update(message_id, length);
  // A clip the person (or a call, or a recording) stopped says nothing: a stop is not a device error.
  if (!by_user) {
    switch (end) {
    case VoicePlayEnd::Unplayable:
      Say(Tr("chat.voice.cannot_play"));
      break;
    case VoicePlayEnd::Busy:
      Say(Tr("chat.voice.play_in_call"));
      break;
    case VoicePlayEnd::NoDevice:
      Say(Tr("chat.voice.no_speaker"));
      break;
    case VoicePlayEnd::Finished:
    case VoicePlayEnd::Stopped:
    case VoicePlayEnd::Interrupted:
      break;
    }
  }
  auto then = std::move(after_stop_);
  after_stop_.clear();
  for (auto& task : then) {
    if (task) {
      task();
    }
  }
}

void VoicePlayback::Update(const std::string& message_id, const std::string& idle_text) {
  if (!ports_.element) {
    return;
  }
  const std::string key = VoiceElementKey(message_id);
  const VoiceBubbleState state = State(message_id);
  if (ui::Element* fill = ports_.element("vf-" + key)) {
    fill->SetProperty("width", std::to_string(VoiceBubbleFillPercent(state)) + "%");
  }
  if (ui::Element* time = ports_.element("vt-" + key)) {
    // The length from the name is only known to the bubble's first draw; back to idle, the clip's own length shows.
    if (state.playing || state.paused) {
      time->SetInnerRML(VoiceBubbleTimeText(state, state.duration_ms));
    } else if (!idle_text.empty()) {
      time->SetInnerRML(idle_text);
    }
  }
  if (ui::Element* button = ports_.element("vp-" + key)) {
    button->SetClass("chat-voice-play--playing", state.playing);
  }
}

} // namespace pbr
