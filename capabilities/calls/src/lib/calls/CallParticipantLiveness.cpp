#include "lib/calls/CallParticipantLiveness.h"

#include "lib/calls/CallSessionLogic.h"

#include <algorithm>
#include <limits>

namespace pbr {

bool PublisherFrameFreshness::Advances(const uint32_t stream_id, const uint16_t channel_id, const uint64_t instance,
                                       const uint32_t seq) {
  ++calls_;
  const Key key{stream_id, channel_id, instance};
  auto it = highest_.find(key);
  if (it == highest_.end()) {
    Evict(stream_id, channel_id);
    highest_.emplace(key, Seen{seq, calls_});
    return true;
  }
  // Serial-number order: a seq at or behind the newest seen is a repeat or a late frame.
  if (static_cast<int32_t>(seq - it->second.seq) <= 0) {
    return false;
  }
  it->second = Seen{seq, calls_};
  return true;
}

void PublisherFrameFreshness::Evict(const uint32_t stream_id, const uint16_t channel_id) {
  const auto first = highest_.lower_bound(Key{stream_id, channel_id, 0});
  const auto last = highest_.upper_bound(Key{stream_id, channel_id, std::numeric_limits<uint64_t>::max()});
  if (static_cast<size_t>(std::distance(first, last)) < kInstancesKept) {
    return;
  }
  const auto stalest =
      std::min_element(first, last, [](const auto& a, const auto& b) { return a.second.touched < b.second.touched; });
  highest_.erase(stalest);
}

void PublisherFrameFreshness::Forget(const uint32_t stream_id) {
  highest_.erase(
      highest_.lower_bound(Key{stream_id, 0, 0}),
      highest_.upper_bound(Key{stream_id, std::numeric_limits<uint16_t>::max(), std::numeric_limits<uint64_t>::max()}));
}

ParticipantLivenessStep ParticipantLivenessWatch::Step(const ParticipantLivenessSample& sample) {
  const int64_t now = sample.now_ms;
  const int64_t gap = last_step_ms_ > 0 ? now - last_step_ms_ : 0;
  last_step_ms_ = now;
  if (live_since_ms_ > 0 && gap >= kParticipantGoneMs) {
    // We sent nothing for as long as the others wait: they have taken us as gone (and the key moved on).
    ParticipantLivenessStep step = Pause(now, sample.our_media_live);
    step.away_too_long = true;
    return step;
  }
  if (!sample.our_media_live || gap > CallSessionLogic::kAppSleptGapMs || gap < 0) {
    return Pause(now, sample.our_media_live);
  }
  if (live_since_ms_ == 0) {
    live_since_ms_ = now;
  }
  ParticipantLivenessStep step;
  std::vector<std::string> quiet;
  std::map<std::string, int64_t> seen;
  std::set<std::string> gone;
  for (const ParticipantLivenessSample::Peer& peer : sample.peers) {
    const auto known = first_seen_.find(peer.identity);
    const int64_t first = known != first_seen_.end() ? known->second : now;
    seen.emplace(peer.identity, first);
    if (gone_.contains(peer.identity)) {
      // Said once; reconnecting on screen until their leave lands (Forget), so the screen can say they lost connection.
      gone.insert(peer.identity);
      quiet.push_back(peer.identity);
      continue;
    }
    const bool heard = peer.last_frame_ms > 0;
    // Never across a pause of ours: the clock counts from our side going live at the earliest.
    const int64_t since = std::max(heard ? peer.last_frame_ms : first, live_since_ms_);
    const int64_t silent_ms = now - since;
    if (silent_ms >= (heard ? kParticipantGoneMs : kParticipantNeverHeardGoneMs)) {
      step.gone.push_back(peer.identity);
      gone.insert(peer.identity);
      quiet.push_back(peer.identity);
    } else if (heard && silent_ms >= kParticipantQuietMs) {
      quiet.push_back(peer.identity);
    }
  }
  first_seen_ = std::move(seen); // who left the call is forgotten
  gone_ = std::move(gone);
  SetQuiet(std::move(quiet), step);
  return step;
}

ParticipantLivenessStep ParticipantLivenessWatch::Pause(const int64_t now_ms, const bool live) {
  live_since_ms_ = live ? now_ms : 0;
  ParticipantLivenessStep step;
  SetQuiet({}, step);
  return step;
}

void ParticipantLivenessWatch::SetQuiet(std::vector<std::string> quiet, ParticipantLivenessStep& step) {
  std::sort(quiet.begin(), quiet.end());
  step.quiet_changed = quiet != quiet_;
  quiet_ = quiet;
  step.quiet = std::move(quiet);
}

void ParticipantLivenessWatch::Forget(const std::string& identity) {
  first_seen_.erase(identity);
  gone_.erase(identity);
  quiet_.erase(std::remove(quiet_.begin(), quiet_.end(), identity), quiet_.end());
}

void ParticipantLivenessWatch::Reset() {
  first_seen_.clear();
  gone_.clear();
  live_since_ms_ = 0;
  last_step_ms_ = 0;
  quiet_.clear();
}

} // namespace pbr
