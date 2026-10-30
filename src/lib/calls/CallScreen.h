#pragma once

#include "lib/calls/CallLifecycleTypes.h"
#include "lib/calls/CallMediaSeat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/**
 * The call surface as the user sees it, decided without a GUI (CallPresenter). The desktop and
 * mobile chrome render it (CallController: words, avatars, video); pp-app-probe reports it to the
 * app lab. Semantic only: no translated text except the call stack's own activity line.
 */
struct CallScreen {
  enum class Kind { None, Ringing, Answering, InCall };
  /** The in-call status line. */
  enum class Status {
    Calling,
    Connecting,
    SettingUpGroup,
    WaitingForMediaPath,
    Connected,
    Reconnecting,
    SendingOnly,
    CouldntConnect,
    /** Was connected, then lost its path for good: the screen closes by itself (B89). */
    Disconnected,
    /** The call stack's own words (`activity`), e.g. a path switch in progress. */
    Activity,
    /** The media engine's connection state as it reads (`raw_state`). */
    Raw,
    /** A group call everyone else left: this device waits for them until `alone_until_ms` (V053). */
    WaitingForOthers,
  };
  /** What the status line explains below itself. */
  enum class Hint { None, P2pConnect, GroupMedia };

  struct Member {
    std::string identity;
    bool is_local = false;
    bool audio_muted = false;
    bool video_enabled = false;
  };

  Kind kind = Kind::None;

  // Ringing / Answering: the invite on screen.
  std::string ring_call_id;
  std::string caller_identity;
  bool ring_video = false;
  /** Ringing over a call this device is in: answering ends that one. */
  bool conflict = false;
  bool conflict_same_peer = false;
  std::string conflict_peer_identity;
  /** P001: the caller's initiation offer (0: none) and whether the take-all answer can be offered. */
  int64_t offer_minor = 0;
  bool charge_enabled = false;
  /** Answering: answered with video (the camera comes on once connected). */
  bool answered_video = false;

  // InCall
  std::string call_id;
  bool video = false;
  /** The roster shows: a group's call, or more than two in it. */
  bool group = false;
  std::vector<Member> members;
  /** The 1:1 peer. */
  std::string peer_identity;
  bool muted = false;
  bool show_speaker = false;
  bool speaker_on = false;
  bool show_retry = false;
  bool show_invite = false;
  bool media_connected = false;
  /** Media was live at some point in this call. */
  bool was_connected = false;
  /** WaitingForOthers: when this device leaves by itself (0: not waiting). */
  int64_t alone_until_ms = 0;
  Status status = Status::Calling;
  std::string activity;
  std::string raw_state;
  /** The call timer (Connected / Reconnecting); 0: none. */
  int64_t connected_at_ms = 0;
  Hint hint = Hint::None;
  bool hint_missing_mic = false;
  bool hint_seed_unreachable = false;

  bool operator==(const CallScreen&) const = default;
};

const char* CallScreenKindName(CallScreen::Kind kind);
const char* CallScreenStatusName(CallScreen::Status status);

/**
 * The status line gives way to "N participants · elapsed": a group's screen with someone in it, while its
 * media is live — and while the group sets itself up again after having been live (someone joining
 * or rejoining moves the media path: SettingUpGroup / WaitingForMediaPath / Connecting), so the one
 * who just came in is not hidden behind "setting up". Lines the user needs to read keep the line:
 * an activity, reconnecting, sending only, couldn't connect, disconnected, or a call not yet live.
 */
bool ShowsParticipantCount(const CallScreen& screen);

/**
 * The call this device was answering or in went away by itself (not by the user's End — that clears
 * the screen first — and not because someone else ended it) while it had never been live and still
 * read as connecting or couldn't connect: the stack gave up and hung up (the attach-wait ran out).
 * The user is told once ("couldn't connect, hung up automatically").
 */
bool EndedWithoutConnecting(const CallScreen& before, const CallScreen& after, bool remote_ended);

/** The group call this device waited alone in went away at the end of the wait: it left by itself. */
bool EndedAlone(const CallScreen& before, const CallScreen& after, int64_t now_ms);

/** A live group call in which nothing arrives from anyone for this long ends (B97). */
inline constexpr int64_t kGroupNoMediaEndMs = 30'000;
/** Media heard this recently counts as arriving. */
inline constexpr int64_t kMediaArrivingWindowMs = 2'000;
/** The sweep that ends the alone wait may run this much after the countdown reads 0:00. */
inline constexpr int64_t kAloneEndLagMs = 2'000;

/** One call's "nothing arrives" count, stepped on every refresh (CallPresenter). */
struct NoMediaWatch {
  std::string call_id;
  /** Since when nothing has arrived (0: not counting). */
  int64_t silent_since_ms = 0;
  int64_t last_step_ms = 0;
};

struct NoMediaSample {
  std::string call_id;
  bool group = false;
  /** The call's media is live (not connecting, moving or reconnecting: those pause the count). */
  bool media_live = false;
  /** Others in the call besides this device (none: the wait-alone rule applies instead). */
  size_t others_joined = 0;
  /** Last audio or video from anyone (0: none yet). A muted peer still sends (silent frames). */
  int64_t last_rx_ms = 0;
  int64_t now_ms = 0;
};

/** True when the call should end: live, others in it, and nothing from any of them for kGroupNoMediaEndMs. */
bool StepNoMediaWatch(NoMediaWatch& watch, const NoMediaSample& sample);

/** What the in-call status decision reads. */
struct InCallSignals {
  /** The 1:1 mesh path could not be set up (the call stack's "couldn't connect"). */
  bool p2p_connect_failed = false;
  bool p2p_missing_mic = false;
  bool p2p_seed_unreachable = false;
  /** Why the hop path is being recovered (None: it is not). */
  CallRecovery recovery = CallRecovery::None;
  bool sfu_mode = false;
  bool group_context = false;
  bool media_chrome_live = false;
  CallMediaStatus media_status = CallMediaStatus::None;
  bool media_active = false;
  /** The engine's connection state ("new", "connecting", "connected", "disconnected", …). */
  std::string connection_state;
  /** The call stack's activity line (empty: none). */
  std::string activity;
  /** Media was live at some point in this call (a later failure is "disconnected", not "couldn't connect"). */
  bool was_connected = false;
};

struct InCallStatus {
  CallScreen::Status status = CallScreen::Status::Calling;
  bool media_connected = false;
  bool show_retry = false;
  /** False: the "add people" button is hidden (a call that could not connect). */
  bool show_invite = true;
  CallScreen::Hint hint = CallScreen::Hint::None;
  /** The activity line is spent (media is live): the stack's activity is to be cleared. */
  bool activity_spent = false;
  /** The call timer runs (Connected / Reconnecting). */
  bool show_elapsed = false;
};

/** The in-call status line and the buttons that go with it (V037: Live wins; activity in flight). */
InCallStatus DecideInCallStatus(const InCallSignals& signals);

} // namespace pbr
