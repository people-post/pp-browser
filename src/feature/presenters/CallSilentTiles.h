#pragma once

#include "common/media_engine/MediaEngineHealth.h"
#include "lib/calls/CallScreen.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Outside the class on purpose: a nested type with default member initializers used in the constructor's default
 * argument is rejected by GCC ("default member initializer required before the end of its enclosing class").
 */
struct CallSilentTilesOptions {
  /**
   * Fallback for an own-path signal that flips too slowly: when two or more people we have received from are all
   * silent for kSilentTileMs, it is us, and nobody new is dimmed. Off until the signals are measured.
   */
  bool all_stop_fallback = false;
};

/**
 * Group call: a participant we have received from, and from whom neither audio nor video has arrived for 15 s, has a
 * dimmed tile that shows the avatar (never a cached picture) and says the connection is lost (DEV/PLAN-call-silent-
 * tile.md). Display only: nobody is ever taken off the roster, nothing is sent. No GUI, no engine, no wall clock.
 *
 * A packet is only an INCREASE of the audio or the video receive counter (kept apart, never summed). A decrease or a
 * reset (the engine rebuilt its tracks) only re-records the reference: it neither restores a dimmed tile nor
 * refreshes the silence timer. Time is the caller's monotonic clock.
 *
 * The silence runs from the latest of the last packet, the person's join and the device's own baseline. A person
 * never received from is never dimmed. The join baseline is taken when a person appears in the list and dropped when
 * they leave. While our own media path is offline nobody new is dimmed; the baseline restarts when it comes back and
 * when two steps are further apart than kTickGapResetMs (the UI was suspended). A dimmed tile stays dimmed until a
 * packet from that person arrives, and shows a picture again only once the video counter has increased since. A
 * person coming back from hold gets a fresh baseline (their media needs a moment to resume).
 *
 * Once a call has been a group call it is tracked for the rest of that call (the joined count may dip to 2), and a
 * refresh whose roster could not be read changes nothing (BeginRefresh).
 */
class CallSilentTiles {
public:
  static constexpr int64_t kSilentTileMs = 15'000;
  static constexpr int64_t kTickGapResetMs = 3'000;

  /** The counters are read (the engine's lock) at most this often; the rules are insensitive to it. */
  static constexpr int64_t kCounterReadMs = 250;

  using Options = CallSilentTilesOptions;

  /** One other person in the joined list, with the engine's counters for their streams (0 when absent). */
  struct Person {
    std::string identity;
    uint64_t audio = 0;
    uint64_t video = 0;
    bool on_hold = false;
  };

  struct Tile {
    /** The dimmed look and the "connection lost" words (not while the person is on hold: one pill only). */
    bool silent = false;
    /** The tile may not show a picture: dimmed, or restored but no video packet since. */
    bool video_blocked = false;
  };

  /** Receives one developer log line per transition. */
  using LogFn = std::function<void(const std::string&)>;

  explicit CallSilentTiles(LogFn log = {}, CallSilentTilesOptions options = {});

  /**
   * One UI refresh of `call_id`: whether to read the counters and Step now. A different call forgets everything first;
   * a roster that could not be read, a call that never was a group call, and a refresh less than kCounterReadMs after
   * the last Step all answer false and leave the state as it is.
   */
  bool BeginRefresh(const std::string& call_id, bool roster_read, bool group, int64_t now_ms);

  /** One UI tick. `others` is everyone joined but us, in any order. */
  void Step(int64_t now_ms, bool own_online, const std::vector<Person>& others);

  /** What to show for `identity` after the last Step (nothing special for someone unknown). */
  Tile TileOf(const std::string& identity) const;

  /** The call changed or ended: everything is forgotten. */
  void Reset();

private:
  struct State {
    std::optional<uint64_t> audio_ref;
    std::optional<uint64_t> video_ref;
    bool ever_received = false;
    int64_t last_progress_ms = 0;
    int64_t join_baseline_ms = 0;
    bool dimmed = false;
    /** Dimmed, and no video packet has come since. */
    bool awaiting_video = false;
    bool on_hold = false;
  };

  enum class Baseline { Packet, Join, Own };
  struct Silence {
    int64_t ms = 0;
    Baseline baseline = Baseline::Packet;
  };
  Silence SilenceOf(const State& s, int64_t now_ms) const;
  static const char* BaselineName(Baseline b);
  static const char* BucketName(int64_t silent_ms);
  void Emit(const char* what, const Silence& silence) const;

  LogFn log_;
  CallSilentTilesOptions options_;
  std::string call_id_;
  /** This call has been a group call (the joined count may later dip to 2). */
  bool was_group_ = false;
  std::map<std::string, State> people_;
  int64_t own_baseline_ms_ = 0;
  int64_t last_step_ms_ = 0;
  bool was_online_ = false;
};

/** The people a step reads: every roster member but us, with the engine's receive counters for their streams. */
std::vector<CallSilentTiles::Person> SilentPeopleFrom(const std::vector<CallScreen::Member>& members,
                                                      const MediaEngineHealth& health);

/** Our own engine is running (a stalled local audio rebuild is not a reason to dim everyone). */
inline bool EngineAlive(const MediaEngineHealth& health) {
  return health.active && health.engine_running;
}

} // namespace pbr
