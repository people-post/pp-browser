#pragma once

#include "common/chat/RelayEnvelope.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** At most this many lane envelopes of one sender wait for its card (ControlLaneHold); its oldest goes first. */
inline constexpr size_t kControlLaneHoldPerSender = 16;
/** At most this many wait in all, whoever sent them; the oldest goes first. */
inline constexpr size_t kControlLaneHoldTotal = 128;
/** How long one waits (this device's clock): long enough for a card ask's answer, short of a call's patience. */
inline constexpr int64_t kControlLaneHoldMaxMs = 2 * 60 * 1000;

/** A lane envelope waiting for its sender's card, and since when (this device's clock, ms). */
struct HeldControlEnvelope {
  RelayEnvelope envelope;
  int64_t held_since_ms = 0;
};

/** What a hold dropped to stay in its bounds. */
struct ControlLaneHoldDrops {
  /** Waited longer than kControlLaneHoldMaxMs. */
  size_t expired = 0;
  /** Its sender's oldest, past kControlLaneHoldPerSender. */
  size_t over_sender = 0;
  /** The oldest of all, past kControlLaneHoldTotal. */
  size_t over_total = 0;
};

/**
 * Control-lane envelopes that could not be checked yet — no card kept for the sender, or the sending device not on
 * the one kept (a device just linked to an account we know; a device of ours just linked, which knows nobody yet) —
 * waiting for the sender's card, then checked again (#555). Bounded per sender, in all and in time, the oldest
 * dropped first. In memory only: the relay has these acked already, so a restart loses them. Pure: the caller
 * locks, and gives the clock.
 */
class ControlLaneHold {
public:
  /**
   * Hold `envelope` from `sender`, waiting since `held_since_ms` (a replayed one keeps its first hold time). True
   * when it waits now — held already counts (the same sender and message id: a copy that came both ways, kept
   * once); false when its time is up.
   */
  bool Hold(const std::string& sender, RelayEnvelope envelope, int64_t held_since_ms, int64_t now_ms,
            ControlLaneHoldDrops* drops = nullptr);
  /** `sender`'s held envelopes still in time, oldest first; they leave the hold. */
  std::vector<HeldControlEnvelope> Take(const std::string& sender, int64_t now_ms,
                                        ControlLaneHoldDrops* drops = nullptr);
  /** Whether anything of `sender`'s waits. */
  bool Holds(const std::string& sender) const;
  size_t Size() const { return entries_.size(); }
  size_t SizeOf(const std::string& sender) const;

private:
  struct Entry {
    std::string sender;
    HeldControlEnvelope held;
  };
  void DropExpired(int64_t now_ms, ControlLaneHoldDrops& drops);
  /** Drops the longest-waiting entry (of `sender`, when given). */
  void DropOldest(const std::string* sender);

  std::deque<Entry> entries_;
};

/**
 * `envelope` as checked again after waiting in the hold since `held_since_ms`: the wait counts as time at the relay
 * (`relay_server_time_ms` moved on by it; through a link, the wait is all the relay time it has), so the lane's
 * freshness (ControlLaneFresh) and a family's own (a call invite's) see how old it really is.
 */
RelayEnvelope AgedByHold(RelayEnvelope envelope, int64_t held_since_ms, int64_t now_ms);

} // namespace pbr
