#pragma once

#include "lib/calls/CallHopAttachLogic.h"
#include "lib/calls/CallMediaBundleLogic.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace pbr {

// V067: every participant of a group call sends a sealed audio frame every 40 ms (silence too) from joining its media
// until it leaves. Each receiver watches every participant on its own by that signal (docs/architecture/CALLS.md
// § Participant liveness).

/** Nothing heard from a participant for this long: their tile reads "Reconnecting…", dimmed. */
inline constexpr int64_t kParticipantQuietMs = 3'000;
/** Nothing heard for this long: they are taken as gone, as a leave would (the 1:1 call's reconnect window). */
inline constexpr int64_t kParticipantGoneMs = kCallMediaReconnectWindowMs;
/** A participant never heard yet (still joining the hop) is not dimmed; gone only after their attach wait and more. */
inline constexpr int64_t kParticipantNeverHeardGoneMs = kSfuAttachWaitDefaultMs + kCallMediaReconnectWindowMs;
/** How often the group path steps the watch (two steps more than CallSessionLogic::kAppSleptGapMs apart: asleep). */
inline constexpr int64_t kParticipantLivenessStepMs = 1'000;

/**
 * Which frames say a participant is alive: one that opened under the call's key (the caller only notes those) and is
 * newer than any seen from that sender instance on that channel. A replay, or an older seq, says nothing — the hop
 * cannot forge a frame, but it could repeat one. Not thread-safe (the caller locks).
 */
class PublisherFrameFreshness {
public:
  /** Sender instances remembered per stream and channel (a restarted sender is a new one); the least recent goes. */
  static constexpr size_t kInstancesKept = 4;

  /** True when this frame is newer than any seen from its sender instance on its channel (and remembers it). */
  bool Advances(uint32_t stream_id, uint16_t channel_id, uint64_t instance, uint32_t seq);
  /** The stream left: nothing of it is kept. */
  void Forget(uint32_t stream_id);
  void Clear() { highest_.clear(); }

private:
  using Key = std::tuple<uint32_t, uint16_t, uint64_t>;
  struct Seen {
    uint32_t seq = 0;
    /** When it last advanced, in Advances calls (which instance is the least recent). */
    uint64_t touched = 0;
  };
  void Evict(uint32_t stream_id, uint16_t channel_id);
  std::map<Key, Seen> highest_;
  uint64_t calls_ = 0;
};

/** One step's input: what this device knows of its side and of each other participant. */
struct ParticipantLivenessSample {
  int64_t now_ms = 0;
  /** Our side carries the call (on the hop, nothing being moved or recovered): else every clock pauses. */
  bool our_media_live = false;
  struct Peer {
    std::string identity;
    /** When a fresh frame (PublisherFrameFreshness) of theirs last arrived; 0: none yet. */
    int64_t last_frame_ms = 0;
  };
  /** The others joined in the call (this device not among them). */
  std::vector<Peer> peers;
};

struct ParticipantLivenessStep {
  /** Who reads "Reconnecting…" now, sorted. */
  std::vector<std::string> quiet;
  bool quiet_changed = false;
  /** Who is gone now (take each as a leave; said once). They stay in `quiet` until forgotten. */
  std::vector<std::string> gone;
  /**
   * This device did not run for a whole gone window while it was in the call (asleep, suspended): everyone else has
   * taken it as gone and moved the key on without it. It leaves (the group's Join takes it back).
   */
  bool away_too_long = false;
};

/**
 * V067: the per-participant clocks of one group call, stepped every kParticipantLivenessStepMs. A participant's clock
 * counts from their last fresh frame, or from when they were first seen joined; never across a time our side was not
 * live (connecting, moving, reconnecting) or asleep — those start every clock over, so our own drop removes nobody.
 */
class ParticipantLivenessWatch {
public:
  ParticipantLivenessStep Step(const ParticipantLivenessSample& sample);
  /** `identity` left the call (any way): its clock goes; it starts again if they rejoin. */
  void Forget(const std::string& identity);
  /** Our part of the call is over (media stopped): everything starts over. */
  void Reset();
  const std::vector<std::string>& Quiet() const { return quiet_; }

private:
  /** Clocks paused (our side not live, or asleep): nobody is quiet, everything counts from `now_ms` on. */
  ParticipantLivenessStep Pause(int64_t now_ms, bool live);
  void SetQuiet(std::vector<std::string> quiet, ParticipantLivenessStep& step);

  /** When each joined participant was first seen (or seen again after a pause). */
  std::map<std::string, int64_t> first_seen_;
  /** Taken as gone (said once), until their leave lands (Forget) or they are no longer joined. */
  std::set<std::string> gone_;
  /** Since when our side has been live without a pause (0: not live). */
  int64_t live_since_ms_ = 0;
  int64_t last_step_ms_ = 0;
  std::vector<std::string> quiet_;
};

} // namespace pbr
