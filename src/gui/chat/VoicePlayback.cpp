#include "gui/chat/VoicePlayback.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppRuntime.h"
#include "gui/chat/VoiceBubble.h"
#include "lib/media/VoiceFile.h"
#include "lib/media/VoiceSupport.h"

#include <ui/dom/Element.h>

namespace pbr {

VoicePlayback::VoicePlayback(VoicePlaybackPorts ports, MediaDeviceArbiter& devices)
    : ports_(std::move(ports)), devices_(devices) {
  const auto alive = alive_;
  VoicePlayerCallbacks callbacks;
  callbacks.on_progress = [this, alive](const std::string& id, const int64_t position_ms, const int64_t duration_ms) {
    AppRuntime::PostUI([this, alive, id, position_ms, duration_ms]() {
      if (alive->load()) {
        OnProgress(id, position_ms, duration_ms);
      }
    });
  };
  callbacks.on_ended = [this, alive](const std::string& id, const VoicePlayEnd end) {
    AppRuntime::PostUI([this, alive, id, end]() {
      if (alive->load()) {
        OnEnded(id, end);
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
  if (!VoiceDeviceBlockedBy(devices_).empty()) {
    Say(Tr("chat.voice.play_in_call")); // not during a call
    return;
  }
  if (!ports_.resolve) {
    return;
  }
  resolving_ = message_id;
  const auto alive = alive_;
  ports_.resolve(message_id, [this, alive, message_id](std::optional<std::string> path) {
    if (!alive->load()) {
      return;
    }
    if (resolving_ == message_id) {
      resolving_.clear();
    }
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
  paused_ = false;
  position_ms_ = 0;
  duration_ms_ = 0;
  if (!before.empty() && before != message_id) {
    Update(before, before_length); // the clip it replaces goes back to idle
  }
  Update(message_id);
  player_->Play(message_id, path, ports_.max_seconds ? ports_.max_seconds() : 120);
}

void VoicePlayback::Stop() {
  if (player_ && !current_.empty()) {
    player_->Stop();
  }
}

void VoicePlayback::OnProgress(const std::string& message_id, const int64_t position_ms, const int64_t duration_ms) {
  if (message_id != current_) {
    return;
  }
  position_ms_ = position_ms;
  duration_ms_ = duration_ms;
  Update(message_id);
}

void VoicePlayback::OnEnded(const std::string& message_id, const VoicePlayEnd end) {
  if (message_id != current_) {
    return; // a clip that was replaced: its bubble was already reset
  }
  const std::string length = duration_ms_ > 0 ? FormatVoiceDuration(duration_ms_) : std::string();
  current_.clear();
  paused_ = false;
  position_ms_ = 0;
  duration_ms_ = 0;
  Update(message_id, length);
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
