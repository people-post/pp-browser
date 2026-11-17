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
 * Two or more others whose last frames came this close together, and nothing since: the call went quiet for this
 * device (its own network), not each of them on their own (V067 8).
 */
inline constexpr int64_t kParticipantsQuietTogetherMs = 2'000;
/**
 * Frames still come and none opens under any key held, this long: the others moved the key on without us (they took
 * us as gone). Longer than a rotation's own gap and a missed key's first resend (2 s), so a late key is not it.
 */
inline constexpr int64_t kShutOutMs = 5'000;
/** How long an ask to rejoin waits for the answer (the join request's own wait, CallSessionLogic). */
inline constexpr int64_t kRejoinWaitMs = 15'000;
/** Asks to rejoin while shut out before this device gives up and leaves ("You were disconnected"). */
inline constexpr int kRejoinAsksMax = 2;

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
  /** The call's people could be read (else nothing is taken as left, and the clocks pause). */
  bool peers_known = true;
  /** When a frame last came that no key we hold opened (from anyone, taken as gone or not); 0: none. */
  int64_t last_unopened_ms = 0;
};

struct ParticipantLivenessStep {
  /** Who reads "Reconnecting…" now, sorted. */
  std::vector<std::string> quiet;
  bool quiet_changed = false;
  /** Who is gone now (take each as a leave; said once). They stay in `quiet` until forgotten. */
  std::vector<std::string> gone;
  /** Watched before and no longer joined (left by any way: a leave, the roster, a removal): their stream goes. */
  std::vector<std::string> left;
  /**
   * This device did not run for a whole gone window while it was in the call (asleep, suspended): everyone else has
   * taken it as gone and moved the key on without it. It leaves (the group's Join takes it back).
   */
  bool away_too_long = false;
  /**
   * V067 8: the call went quiet for this device, not one of them for everyone — two or more others stopped together,
   * or frames come that no key we hold opens. Nobody is dimmed or taken as gone meanwhile; the call reads
   * "Reconnecting…".
   */
  bool cut_off = false;
  bool cut_off_changed = false;
  /** Cut off long enough that the others have taken us as gone (or shown it: the key moved on): ask to rejoin. */
  bool rejoin = false;
  /** Shut out and the asks to rejoin went unanswered: leave ("You were disconnected"; the group's Join is there). */
  bool disconnected = false;
};

/**
 * V067: the per-participant clocks of one group call, stepped every kParticipantLivenessStepMs. A participant's clock
 * counts from their last fresh frame, or from when they were first seen joined; never across a time our side was not
 * live (connecting, moving, reconnecting) or asleep — those start every clock over, so our own drop removes nobody.
 * Nor while the call went quiet for this device as a whole (V067 8: `cut_off`): then it asks to rejoin once the others
 * must have taken it as gone, and leaves only when shut out and unanswered; when nothing at all came back and the ask
 * went unanswered, the others may really have gone together, and each is judged on their own (V053's wait alone).
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
  void Pause(int64_t now_ms, bool live, ParticipantLivenessStep& step);
  /** Who was watched and is no longer among `peers` goes into `step.left` (and is forgotten). */
  void TakeLeft(const std::vector<ParticipantLivenessSample::Peer>& peers, ParticipantLivenessStep& step);
  /** Our side is live: each participant's clock. */
  void StepLive(const ParticipantLivenessSample& sample, ParticipantLivenessStep& step);
  /** Our side is live: whether the call went quiet for this device (then nobody is judged on their own), and its asks. */
  bool StepCutOff(const ParticipantLivenessSample& sample, ParticipantLivenessStep& step);
  /** Cut off while still asking to rejoin; false: it gave up (said in `step`), or nothing at all came back (V053). */
  bool StepRejoinWait(int64_t now_ms, bool shut_out, ParticipantLivenessStep& step);
  /** Asks to rejoin (said in `step`). */
  void AskRejoin(int64_t now_ms, ParticipantLivenessStep& step);
  /** Frames open again: whatever was said of being cut off starts over. */
  void ClearCutOff();
  bool SetCutOff(bool cut_off, ParticipantLivenessStep& step);
  /** Two or more others (heard, not taken as gone) whose last frames came within kParticipantsQuietTogetherMs. */
  bool MayBeQuietTogether(const ParticipantLivenessSample& sample) const;
  void SetQuiet(std::vector<std::string> quiet, ParticipantLivenessStep& step);

  /** When each joined participant was first seen (or seen again after a pause). */
  std::map<std::string, int64_t> first_seen_;
  /** Taken as gone (said once), until their leave lands (Forget) or they are no longer joined. */
  std::set<std::string> gone_;
  /** Since when our side has been live without a pause (0: not live). */
  int64_t live_since_ms_ = 0;
  int64_t last_step_ms_ = 0;
  std::vector<std::string> quiet_;
  bool cut_off_ = false;
  /** Since when frames come that no key we hold opens, with nothing opening (0: not so). */
  int64_t shut_out_since_ms_ = 0;
  /** When we last asked to rejoin (0: not asked since frames last opened), and how often. */
  int64_t rejoin_asked_ms_ = 0;
  int rejoin_asks_ = 0;
  /** The others went quiet together, our ask went unanswered and nothing came back: they may have; each is judged. */
  bool together_given_up_ = false;
  bool disconnected_said_ = false;
};

} // namespace pbr
