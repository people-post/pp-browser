#pragma once

#include "common/media/CallMediaHealth.h"
#include "feature/presenters/CallConnectStory.h"
#include "feature/presenters/CallMediaHintClock.h"
#include "feature/presenters/CallReconnectCapsule.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallScreen.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/**
 * One tick's facts about the media of the call on screen, read once from the call stack and the engine (CallPresenter
 * reads them after the engine's video upkeep; the UI demo makes them up). Everything CallMediaFrame decides from.
 */
struct CallMediaFacts {
  std::string call_id;
  int64_t now_ms = 0;

  // --- The call (CallUiBackend).
  /** VideoAllowedForCall: none when it is not known or could not be read. */
  std::optional<bool> video_allowed;
  /** The call was placed or answered with video (CallPresenter::VideoIntentCallId). */
  bool video_intent = false;
  /** Still ringing the others (CallPhase::OutboundCalling). */
  bool ringing_out = false;
  bool media_chrome_live = false;
  CallMediaStatus media_status = CallMediaStatus::None;
  /** The 1:1 mesh path could not be set up; a hop recovery in flight; the call on the group (hop) path. */
  bool direct_connect_failed = false;
  bool awaiting_sfu_recovery = false;
  bool on_hop = false;
  /** When the call first went live (0: never): the call timer's start. */
  int64_t live_since_ms = 0;

  // --- The 1:1 peer's roster row (PeerMediaForCall; none before the peer is known).
  bool have_peer_row = false;
  bool peer_camera_on = false;
  bool peer_on_hold = false;
  bool peer_video_paused = false;

  // --- The engine (IMediaView).
  bool media_active = false;
  bool camera_on = false;
  bool encoder_available = false;
  /** The path allows video now (CallMediaAdaptation::CameraPathAllowsVideo). */
  bool path_allows_video = false;
  bool has_remote_video = false;
  bool remote_video_stalling = false;
  /** A remote frame within the last kPeerVideoLiveMs. */
  bool peer_frames_live = false;
  bool ever_had_remote_video = false;

  /** V060: the OS has this device's call audio (CallLocalHold). */
  bool local_on_hold = false;

  // --- What the media health is evaluated from (EvaluateCallMediaHealth).
  MediaEngineHealth engine;
  CallHopHealth hop;
  std::string reach_path_kind;
  /** The call's link counters now (sampled every CallMediaFrame::kLinkSampleMs). */
  CallLinkCounters link_counters;
};

/** What the remote video stage says where there is no picture. */
enum class CallStagePlaceholder { None, StartingVideo, CameraOff, CameraPaused };

/** The media decisions of one tick. */
struct CallMediaDecision {
  /** The peer's camera: its roster row, overruled by live frames (B58). */
  bool peer_camera_on = false;
  /** The roster says the peer's camera is off and no frame says otherwise: its last picture goes. */
  bool clear_remote_video = false;
  /** A remote picture shows (a fresh frame, and the peer's camera is meant to be on). */
  bool remote_video = false;
  bool remote_stalling = false;
  /** Remote video came and is missing now while it is expected (a hard stall). */
  bool video_missing = false;
  /** The 1:1 path could not be set up: "couldn't connect" (a call that was live reads "disconnected" instead). */
  bool couldnt_connect = false;
  /** The call's path is gone (CallMediaStatus::Reconnecting). */
  bool path_lost = false;
  /** The call's media was live and is not now while the call runs (it reconnects or moves path). */
  bool media_dropped = false;
  /** The call reconnects (the capsule): a lost path, or — not on hold — a stall, missing video or dropped media. */
  bool reconnect = false;
  /** V060: on hold here (the OS has the audio), or here or there. Silence by design, not a broken line. */
  bool local_hold = false;
  bool on_hold = false;
  /** A video call waits for the peer's first frame: the stage shows "Starting video…" (at most kVideoStartMs). */
  bool starting_video = false;
  /** The video stage shows (the GUI adds its local preview to it). */
  bool stage_visible = false;
  CallStagePlaceholder placeholder = CallStagePlaceholder::None;
  /** The camera button (ShowsCameraButton: a camera that is on can always be turned off). */
  bool show_camera = false;
  /** V035: the call was narrowed to voice while the camera is on: turn it off (once per narrowing). */
  bool narrow_camera = false;

  bool operator==(const CallMediaDecision&) const = default;
};

/**
 * The in-call status line, decided in one place (#181); the GUI words it with the tick's clock. `key` is a locale key;
 * the kind says what fills it.
 */
struct CallStatusLine {
  enum class Kind {
    /** `key`'s words. */
    Words,
    /** The call timer from `since_ms`; `key` ("Connected", or "" for nothing) while there is no timer. */
    Timer,
    /** The call stack's own words (CallScreen::activity). */
    Activity,
    /** `key` with {time}: what is left until `until_ms` (V053: waiting alone for the others). */
    Countdown,
    /** `key` with {count} (`count`) and {elapsed} (the call timer from `since_ms`). */
    Participants,
    /** V060: `key` with {name}, the 1:1 peer; `fallback_key` when their name is not known. */
    PeerOnHold,
  };
  Kind kind = Kind::Words;
  std::string key;
  std::string fallback_key;
  int64_t since_ms = 0;
  int64_t until_ms = 0;
  int count = 0;
  /** The desktop's path word after the line ("… · Direct", CallPathWordKey); "" none. Phones leave it out. */
  std::string path_key;
  /** Couldn't connect: what the line explains below itself. */
  CallScreen::Hint hint = CallScreen::Hint::None;

  bool operator==(const CallStatusLine&) const = default;
};

/** One tick of the call on screen's media, as the GUI renders it and pp-app-probe reads it. */
struct CallMediaScreen {
  std::string call_id;
  int64_t now_ms = 0;
  /** An in-call tick (the facts were read); false while answering (only the story and the capsule step). */
  bool in_call = false;
  CallMediaDecision media;
  CallMediaHealthView health;
  /** The call timer's start (0: no timer). */
  int64_t timer_since_ms = 0;
  CallStatusLine line;
  /** The one-way-audio hint (CallMediaHintClock; "" none), and whether the hint line shows it (not on hold). */
  std::string hint_key;
  bool hint_shown = false;
  CallStoryView story;
  CallReconnectView reconnect;
};

/**
 * The call screen's media decisions, without a GUI (CallPresenter owns one; pp-app-probe reads it through the
 * presenter): from one tick's facts, whether the call reconnects, what the status line says, what the video stage
 * shows and when the camera narrows, and the clocks behind them — the "Starting video…" window, the one-way-audio hint
 * (CallMediaHintClock), the connection story (CallConnectStory) and the reconnect capsule (CallReconnectCapsule).
 */
class CallMediaFrame {
public:
  /** A video call shows "Starting video…" at most this long after connect while it waits for the first frame. */
  static constexpr int64_t kVideoStartMs = 10'000;
  /** The link is sampled this often (its resend share is a delta between two samples). */
  static constexpr int64_t kLinkSampleMs = 2'000;

  /** An in-call tick: the facts' health (the link sampled) and the decisions. */
  const CallMediaScreen& Step(const CallScreen& screen, const CallMediaFacts& facts);
  /** The same with the media health given (the UI demo's made-up one; tests). */
  const CallMediaScreen& Step(const CallScreen& screen, const CallMediaFacts& facts, const CallMediaHealthView& health);
  /** Answering (no media yet): only the story and the capsule step. */
  const CallMediaScreen& StepAnswering(const CallScreen& screen, int64_t now_ms);
  /** No call: everything starts over. */
  void Reset();

  const CallMediaScreen& View() const { return view_; }

private:
  /** The decisions from the facts (a new call starts its windows over). */
  CallMediaDecision Decide(const CallScreen& screen, const CallMediaFacts& facts);
  /** The hint, the status line, the story and the capsule from the decisions and the health. */
  const CallMediaScreen& Finish(const CallScreen& screen, const CallMediaFacts& facts, const CallMediaDecision& d,
                                const CallMediaHealthView& health);
  bool StartingVideo(const CallMediaFacts& facts);
  CallLinkHealth SampleLink(const CallMediaFacts& facts);
  void StepStory(const CallStoryInput& in, bool reconnecting);

  CallMediaScreen view_;
  std::string call_id_;
  /** When the call's media came live while its video was still starting (0: not yet). */
  int64_t video_start_since_ms_ = 0;
  /** V035: the camera was turned off for this narrowing (again only once video is allowed again). */
  bool camera_narrowed_ = false;
  CallLinkCounters link_counters_;
  CallLinkHealth link_health_;
  int64_t link_sampled_ms_ = 0;
  CallMediaHintClock hint_clock_;
  CallConnectStory story_;
  CallReconnectCapsule reconnect_;
};

} // namespace pbr
