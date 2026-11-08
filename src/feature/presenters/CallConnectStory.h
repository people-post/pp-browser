#pragma once

#include "common/media/CallMediaHealth.h"
#include "lib/calls/CallScreen.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

/**
 * The call screen's "connection story" (DESIGN-pp-ui 8.12, 2026-11-04; mockup 08-call-connect-story; TODO #76):
 * what the phone call page tells the user about how the call got through, decided without a GUI. Only steps that
 * really happened, in plain words: no "punched / relay / NAT", no error codes, no latency figures (those stay in the
 * log and the diagnostics view).
 *
 * Fed each poll with what the screen already knows (CallScreen and CallMediaHealthView); nothing new from the
 * connection layer. The live "trying direct, then around the limits" progress needs events the connection layer does
 * not report yet (DEV/NOTE-call-connect-progress-api.md), so before connect only calling / answered / connecting show.
 */

/** How the call got through, in the user's words. Only trusted once the call is connected. */
enum class CallRoute { Unknown, Direct, AroundLimits, Relayed };

/** `CallMediaHealthView::path_kind` as a route: direct, punched (around the limits), circuit / media_relay (relayed). */
CallRoute CallRouteFromPathKind(const std::string& path_kind);

/** The signal bar's colour: secure when good, warn with its word, danger with its word. */
enum class CallSignalTone { Good, Fair, Poor };

/** The top pill (DESIGN-pp-ui 8.11) the story raises on a real change. */
enum class CallStoryNotice { None, Slow, Closer };

struct CallStoryInput {
  std::string call_id;
  /** Answering (this device answered, media not up yet) or InCall. */
  CallScreen::Kind kind = CallScreen::Kind::InCall;
  CallScreen::Status status = CallScreen::Status::Calling;
  bool group = false;
  /** This device started the call (CallScreen::placed_here); else it answered or joined. */
  bool placed_here = false;
  /** The call's media is live now. */
  bool media_connected = false;
  std::string path_kind;
  CallPathQuality quality = CallPathQuality::Good;
  int quality_bars = 4;
  int64_t now_ms = 0;
};

/** One line of the "how this call got through" timeline. */
struct CallStoryEvent {
  enum class Kind {
    /** This device placed the call. */
    Placed,
    /** This device answered (or joined a group's call). */
    Answered,
    /** The other side answered (only an outgoing 1:1 call shows this). */
    PeerAnswered,
    /** Media came up: `route` says how. */
    Connected,
    /** The call is end-to-end encrypted (said with Connected: there is no separate "keys ready" event). */
    Encrypted,
    /** A relayed call moved onto a nearer, direct path. */
    Closer,
  };
  Kind kind = Kind::Placed;
  /** Since the story began (this device placed or answered the call). The first line, and lines of a call this
   *  device found already live, show `wall_ms` as a time of day instead. */
  int64_t at_ms = 0;
  /** Since the call connected (Closer); -1 before. */
  int64_t call_ms = -1;
  CallRoute route = CallRoute::Unknown;
  /** When it happened (unix ms). */
  int64_t wall_ms = 0;

  bool operator==(const CallStoryEvent&) const = default;
};

struct CallStoryView {
  // --- Before connect: the moving line, the steps and the rotating tips.
  bool connecting = false;
  /** The stage on screen (the ones before it are done). A group call: 0 the group is being called, 1 joined and the
   *  line is being set up. A 1:1 call (the five steps, #172): 0 calling, 1 the other side answered, 2 matching the
   *  fastest line (the time between the answer and live media), 3 setting up the encrypted channel, 5 everything
   *  done, "connected" included (4 is never shown: connected is only ever the finished list). Follows the real
   *  stage, but each stage stays on screen at least kStepMinShowMs, and the finished list stays kListHoldMs. */
  int stage = 0;
  /** This device placed the call (the first step reads "Calling X…" then "X answered"). */
  bool outgoing = false;

  // --- Just connected (1:1): one line under the name for kResultShowMs, then it fades.
  bool result_shown = false;
  bool result_fading = false;
  CallRoute result_route = CallRoute::Unknown;
  /** Answer (or this device's accept) to media up, in tenths of a second. */
  int64_t connect_tenths = 0;

  // --- In call: the signal bar (never while the result line shows).
  bool signal_shown = false;
  int signal_bars = 4;
  CallSignalTone signal_tone = CallSignalTone::Good;
  /** The quality word next to the bars: empty when good. */
  CallPathQuality signal_quality = CallPathQuality::Good;

  // --- The top pill.
  CallStoryNotice notice = CallStoryNotice::None;
  bool notice_fading = false;

  /** How the call goes now (Unknown before it connected). */
  CallRoute route = CallRoute::Unknown;
  std::vector<CallStoryEvent> events;

  bool operator==(const CallStoryView&) const = default;
};

class CallConnectStory {
public:
  static constexpr int64_t kResultShowMs = 4000;
  static constexpr int64_t kResultFadeMs = 400;
  static constexpr int64_t kNoticeShowMs = 3000;
  static constexpr int64_t kNoticeFadeMs = 300;
  /** The signal bar changes at most this often (B146: no per-frame redraw). */
  static constexpr int64_t kSignalHoldMs = 500;
  /** "The line got slower" is said at most this often. */
  static constexpr int64_t kSlowNoticeEveryMs = 30'000;
  /** After media connects the bar waits this long at most for audio to settle (or the first good sample). */
  static constexpr int64_t kSettleMs = 2'000;
  /** Each step of a 1:1 call's list stays on screen at least this long (only the text waits, never the call). */
  static constexpr int64_t kStepMinShowMs = 450;
  /** After "connected" the finished list stays this long before the call page takes over. */
  static constexpr int64_t kListHoldMs = 1'500;
  /** Stage values of a 1:1 call (CallStoryView::stage). */
  static constexpr int kStageCalling = 0;
  static constexpr int kStageAnswered = 1;
  static constexpr int kStageMatching = 2;
  static constexpr int kStageSecuring = 3;
  static constexpr int kStageDone = 5;
  /** Steps the story with this poll's facts and returns what the call page shows now. */
  const CallStoryView& Step(const CallStoryInput& in);
  const CallStoryView& View() const { return view_; }
  void Reset();

private:
  void Raise(CallStoryNotice notice, int64_t now_ms);

  CallStoryView view_;
  std::string call_id_;
  bool answered_ = false;
  bool connected_ = false;
  bool slow_noticed_ = false;
  bool settled_ = false;
  int64_t began_ms_ = 0;
  int64_t answered_ms_ = 0;
  int64_t connected_ms_ = 0;
  /** The 1:1 list: when the shown stage began, and when the finished list began its hold. */
  int64_t stage_began_ms_ = 0;
  int64_t done_ms_ = 0;
  int64_t notice_ms_ = 0;
  int64_t slow_notice_ms_ = 0;
  int64_t signal_changed_ms_ = 0;
  CallScreen::Status last_status_ = CallScreen::Status::Calling;
  CallSignalTone last_tone_ = CallSignalTone::Good;
};

/** The just-connected line: "Connected directly" / "Connected around network limits" / "…through an encrypted relay". */
const char* CallRouteResultKey(CallRoute route);
/** The timeline's "connected" line for a 1:1 call. */
const char* CallRouteEventKey(CallRoute route);
/** The sheet's "how" row. */
const char* CallRouteHowKey(CallRoute route);
/** The pill's words (empty for None). */
const char* CallStoryNoticeKey(CallStoryNotice notice);
/** The word next to the signal bars (empty when good). */
const char* CallSignalWordKey(CallPathQuality quality);
/** The four true loading tips, shown in turn every 3 s before connect. */
inline constexpr const char* kCallStoryTipKeys[] = {
    "call.story.tip.encrypted",
    "call.story.tip.no_vpn",
    "call.story.tip.reroute",
    "call.story.tip.closer",
};

} // namespace pbr
