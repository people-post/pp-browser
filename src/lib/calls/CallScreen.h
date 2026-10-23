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

/** What the in-call status decision reads. */
struct InCallSignals {
  /** The 1:1 mesh path could not be set up (the call stack's "couldn't connect"). */
  bool p2p_connect_failed = false;
  bool p2p_missing_mic = false;
  bool p2p_seed_unreachable = false;
  bool awaiting_sfu_recovery = false;
  bool sfu_mode = false;
  bool group_context = false;
  bool media_chrome_live = false;
  CallMediaSeat::MediaState seat_media = CallMediaSeat::MediaState::Idle;
  CallMediaStatus media_status = CallMediaStatus::None;
  bool media_active = false;
  bool soft_migrate_in_flight = false;
  bool sfu_attach_wait_active = false;
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
