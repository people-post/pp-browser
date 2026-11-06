#include "gui/chat/VoicePlayback.h"

#include "foundation/i18n/LocalizationService.h"
#include "common/Logger.h"
#include "common/runtime/AppRuntime.h"
#include "gui/chat/VoiceBubble.h"
#include "lib/voice/VoiceAudioSession.h"
#include "lib/voice/VoiceFile.h"

#include <ui/dom/Element.h>

namespace pbr {

namespace {

logging::Logger& Log() {
  static logging::Logger log = logging::getLogger("VoicePlayback");
  return log;
}

} // namespace

VoicePlayback::VoicePlayback(VoicePlaybackPorts ports, MediaDeviceArbiter& devices)
    : ports_(std::move(ports)), devices_(devices) {
  if (!ports_.post_ui) {
    ports_.post_ui = [](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); };
  }
  // The clip thread may outlive this object (Shutdown stops the player without joining): its callbacks hold their
  // own copy of `post` and touch `this` only on the UI thread, after the alive check.
  const auto alive = alive_;
  const auto post = ports_.post_ui;
  VoicePlayerCallbacks callbacks;
  callbacks.on_progress = [this, alive, post](const std::string& id, const uint64_t serial, const int64_t position_ms,
                                              const int64_t duration_ms) {
    post([this, alive, id, serial, position_ms, duration_ms]() {
      if (alive->load()) {
        OnProgress(id, serial, position_ms, duration_ms);
      }
    });
  };
  callbacks.on_ended = [this, alive, post](const std::string& id, const uint64_t serial, const VoicePlayEnd end) {
    post([this, alive, id, serial, end]() {
      if (alive->load()) {
        OnEnded(id, serial, end);
      }
    });
  };
  player_ = std::make_unique<VoicePlayer>([this]() { return CreateArbiterVoicePlaybackSink(devices_); },
                                          std::move(callbacks));
  file_player_ = ports_.make_file_player ? ports_.make_file_player() : CreatePlatformAudioFilePlayer();
  if (!ports_.file_session) {
    ports_.file_session = [](const bool playing) {
      if (playing) {
        VoiceAudioSession::ActivateForPlayback();
      } else {
        VoiceAudioSession::Release();
      }
    };
  }
}

VoicePlayback::~VoicePlayback() {
  Shutdown();
}

void VoicePlayback::Shutdown() {
  alive_->store(false);
  player_.reset();
  if (file_player_) {
    StopFile();
    file_player_.reset();
  }
}

void VoicePlayback::StopFile() {
  if (file_player_ && current_is_file_ && !current_.empty()) {
    file_player_->Stop();
    ports_.file_session(false);
  }
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
  ToggleImpl(message_id, /*file=*/false);
}

void VoicePlayback::ToggleFile(const std::string& message_id) {
  if (file_player_) {
    ToggleImpl(message_id, /*file=*/true);
  }
}

void VoicePlayback::ToggleImpl(const std::string& message_id, const bool file) {
  if (message_id.empty() || !player_ || message_id == resolving_) {
    return;
  }
  if (message_id == current_) {
    if (paused_) {
      current_is_file_ ? file_player_->Resume() : player_->Resume();
      paused_ = false;
    } else {
      current_is_file_ ? file_player_->Pause() : player_->Pause();
      paused_ = true;
    }
    Update(message_id);
    return;
  }
  if (ports_.call_live && ports_.call_live()) {
    if (file) {
      Log().info << "sound attachment tap: route=in-app player, not played: a call is live";
    }
    Say(Tr("chat.voice.play_in_call")); // not during a call
    return;
  }
  if (!ports_.resolve) {
    return;
  }
  if (file) {
    // The next device log tells a tap that never got here from one the player could not start (#111).
    Log().info << "sound attachment tap: route=in-app player";
  }
  resolving_ = message_id;
  const uint64_t generation = ++resolve_generation_;
  const auto alive = alive_;
  ports_.resolve(message_id, [this, alive, message_id, generation, file](std::optional<std::string> path) {
    if (!alive->load()) {
      return;
    }
    // A Stop (or another tap) in the meantime cancelled this one: its file is not played late.
    if (generation != resolve_generation_ || resolving_ != message_id) {
      return;
    }
    resolving_.clear();
    if (!path) {
      if (file) {
        Log().warning << "sound attachment: no file to play (the stored copy could not be opened)";
      }
      Say(Tr(file ? "chat.attachment.audio_cannot_play" : "chat.voice.cannot_play"));
      return;
    }
    Start(message_id, *path, file);
  });
}

void VoicePlayback::Start(const std::string& message_id, const std::string& path, const bool file) {
  const std::string before = current_;
  const std::string before_length = duration_ms_ > 0 ? FormatVoiceDuration(duration_ms_) : std::string();
  // One at a time across both players: a file playing goes now; a voice clip is replaced by the voice player itself,
  // or stopped here when a file takes over (its late end is ignored: another id is current by then).
  StopFile();
  if (file && !before.empty() && !current_is_file_) {
    player_->Stop();
  }
  current_is_file_ = file;
  current_ = message_id;
  stopped_by_user_ = false;
  paused_ = false;
  position_ms_ = 0;
  duration_ms_ = 0;
  if (!before.empty() && before != message_id) {
    Update(before, before_length); // the clip it replaces goes back to idle
  }
  Update(message_id);
  if (!file) {
    serial_ = player_->Play(message_id, path, ports_.max_seconds ? ports_.max_seconds() : 120);
    return;
  }
  serial_ = (uint64_t{1} << 63) | ++file_serial_;
  const uint64_t serial = serial_;
  const auto alive = alive_;
  const auto post = ports_.post_ui;
  AudioFilePlayerCallbacks callbacks;
  callbacks.on_progress = [this, alive, post, message_id, serial](const int64_t position_ms, const int64_t duration_ms) {
    post([this, alive, message_id, serial, position_ms, duration_ms]() {
      if (alive->load()) {
        OnProgress(message_id, serial, position_ms, duration_ms);
      }
    });
  };
  callbacks.on_ended = [this, alive, post, message_id, serial](const VoicePlayEnd end) {
    post([this, alive, message_id, serial, end]() {
      if (alive->load()) {
        OnEnded(message_id, serial, end);
      }
    });
  };
  ports_.file_session(true); // loudspeaker, also with the silent switch on (not the earpiece a call left)
  if (!file_player_->Play(path, std::move(callbacks))) {
    Log().warning << "sound attachment: the player did not start";
    OnEnded(message_id, serial, VoicePlayEnd::Unplayable);
  }
}

void VoicePlayback::Stop() {
  resolving_.clear();
  ++resolve_generation_;
  if (player_ && !current_.empty()) {
    stopped_by_user_ = true;
    if (current_is_file_) {
      // The system's player says nothing after a Stop: its end is reported here, on the next UI turn (after a
      // StopThen has queued what waits for it). That end hands the session back, once: not here too (a second
      // release could end a session a call took in between).
      file_player_->Stop();
      const auto alive = alive_;
      ports_.post_ui([this, alive, id = current_, serial = serial_]() {
        if (alive->load()) {
          OnEnded(id, serial, VoicePlayEnd::Stopped);
        }
      });
      return;
    }
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
  const bool was_file = current_is_file_;
  if (current_is_file_) {
    if (end != VoicePlayEnd::Interrupted) {
      ports_.file_session(false); // taken over by another player: the session is that one's now
    }
    current_is_file_ = false;
  }
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
      Say(Tr(was_file ? "chat.attachment.audio_cannot_play" : "chat.voice.cannot_play"));
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
