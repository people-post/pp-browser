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
  const auto first = highest_.lower_bound(Key{stream_id, channel_id, uint64_t{0}});
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
      highest_.lower_bound(Key{stream_id, uint16_t{0}, uint64_t{0}}),
      highest_.upper_bound(Key{stream_id, std::numeric_limits<uint16_t>::max(), std::numeric_limits<uint64_t>::max()}));
}

bool ParticipantLivenessWatch::MayBeQuietTogether(const ParticipantLivenessSample& sample) const {
  if (together_given_up_) {
    return false;
  }
  int64_t earliest = 0;
  int64_t latest = 0;
  size_t heard = 0;
  for (const ParticipantLivenessSample::Peer& peer : sample.peers) {
    if (peer.last_frame_ms <= 0 || gone_.contains(peer.identity)) {
      continue;
    }
    earliest = heard == 0 ? peer.last_frame_ms : std::min(earliest, peer.last_frame_ms);
    latest = std::max(latest, peer.last_frame_ms);
    ++heard;
  }
  return heard >= 2 && latest - earliest <= kParticipantsQuietTogetherMs;
}

ParticipantLivenessStep ParticipantLivenessWatch::Step(const ParticipantLivenessSample& sample) {
  const int64_t now = sample.now_ms;
  const int64_t gap = last_step_ms_ > 0 ? now - last_step_ms_ : 0;
  last_step_ms_ = now;
  ParticipantLivenessStep step;
  if (sample.peers_known) {
    TakeLeft(sample.peers, step);
  }
  if (live_since_ms_ > 0 && gap >= kParticipantGoneMs) {
    // We sent nothing for as long as the others wait: they have taken us as gone (and the key moved on).
    Pause(now, sample.our_media_live, step);
    step.away_too_long = true;
  } else if (!sample.our_media_live || !sample.peers_known || gap > CallSessionLogic::kAppSleptGapMs || gap < 0) {
    Pause(now, sample.our_media_live && sample.peers_known, step);
  } else {
    StepLive(sample, step);
  }
  return step;
}

void ParticipantLivenessWatch::TakeLeft(const std::vector<ParticipantLivenessSample::Peer>& peers,
                                        ParticipantLivenessStep& step) {
  for (auto it = first_seen_.begin(); it != first_seen_.end();) {
    const bool joined = std::any_of(peers.begin(), peers.end(),
                                    [&](const ParticipantLivenessSample::Peer& p) { return p.identity == it->first; });
    if (joined) {
      ++it;
      continue;
    }
    step.left.push_back(it->first);
    gone_.erase(it->first);
    it = first_seen_.erase(it);
  }
}

void ParticipantLivenessWatch::StepLive(const ParticipantLivenessSample& sample, ParticipantLivenessStep& step) {
  const int64_t now = sample.now_ms;
  if (live_since_ms_ == 0) {
    live_since_ms_ = now;
  }
  if (StepCutOff(sample, step)) {
    SetQuiet({}, step); // it is us: nobody is dimmed, nobody's clock decides
    return;
  }
  // Dimmed only once it cannot be us: while the others' last frames are within a moment of each other, the next steps
  // may still find everyone quiet together (StepCutOff).
  const bool may_be_us = MayBeQuietTogether(sample);
  std::vector<std::string> quiet;
  for (const ParticipantLivenessSample::Peer& peer : sample.peers) {
    const int64_t first = first_seen_.emplace(peer.identity, now).first->second;
    if (gone_.contains(peer.identity)) {
      // Said once; reconnecting on screen until their leave lands (Forget), so the screen can say they lost connection.
      quiet.push_back(peer.identity);
      continue;
    }
    const bool heard = peer.last_frame_ms > 0;
    // Never across a pause of ours: the clock counts from our side going live at the earliest.
    const int64_t silent_ms = now - std::max(heard ? peer.last_frame_ms : first, live_since_ms_);
    if (silent_ms >= (heard ? kParticipantGoneMs : kParticipantNeverHeardGoneMs)) {
      step.gone.push_back(peer.identity);
      gone_.insert(peer.identity);
      quiet.push_back(peer.identity);
    } else if (heard && silent_ms >= kParticipantQuietMs && !may_be_us) {
      quiet.push_back(peer.identity);
    }
  }
  SetQuiet(std::move(quiet), step);
}

bool ParticipantLivenessWatch::StepCutOff(const ParticipantLivenessSample& sample, ParticipantLivenessStep& step) {
  const int64_t now = sample.now_ms;
  int64_t latest = 0; // the last frame that opened, from anyone
  for (const ParticipantLivenessSample::Peer& peer : sample.peers) {
    latest = std::max(latest, peer.last_frame_ms);
  }
  const int64_t opened_since = std::max(latest, live_since_ms_);
  if (now - opened_since < kParticipantQuietMs) {
    ClearCutOff(); // frames open: we are in the call
    return SetCutOff(false, step);
  }
  // Frames that come and do not open: the others are there, sealing with a key we lack.
  const bool unopened = sample.last_unopened_ms > 0 && sample.last_unopened_ms >= live_since_ms_ &&
                        now - sample.last_unopened_ms < kParticipantQuietMs;
  shut_out_since_ms_ = unopened ? (shut_out_since_ms_ > 0 ? shut_out_since_ms_ : now) : 0;
  const bool shut_out = shut_out_since_ms_ > 0;
  // Two or more stopping at once: our network, far likelier than theirs going in the same moment.
  const bool together = MayBeQuietTogether(sample);
  if (!shut_out && !together) {
    return SetCutOff(false, step);
  }
  if (rejoin_asked_ms_ > 0) {
    return SetCutOff(StepRejoinWait(now, shut_out, step), step);
  }
  // The others take us as gone at their own window (as we would them), and then the key moves on without us.
  if ((shut_out && now - shut_out_since_ms_ >= kShutOutMs) || (together && now - opened_since >= kParticipantGoneMs)) {
    AskRejoin(now, step);
  }
  return SetCutOff(true, step);
}

bool ParticipantLivenessWatch::StepRejoinWait(const int64_t now_ms, const bool shut_out,
                                              ParticipantLivenessStep& step) {
  if (now_ms - rejoin_asked_ms_ < kRejoinWaitMs) {
    return true;
  }
  if (!shut_out) {
    // Nothing came back at all: they may really have gone together. Each is judged on their own now (V053).
    together_given_up_ = true;
    rejoin_asked_ms_ = 0;
    return false;
  }
  if (rejoin_asks_ < kRejoinAsksMax) {
    AskRejoin(now_ms, step); // the others are there (their frames come): ask again
  } else if (!disconnected_said_) {
    disconnected_said_ = true;
    step.disconnected = true;
  }
  return true;
}

void ParticipantLivenessWatch::AskRejoin(const int64_t now_ms, ParticipantLivenessStep& step) {
  step.rejoin = true;
  rejoin_asked_ms_ = now_ms;
  ++rejoin_asks_;
}

void ParticipantLivenessWatch::ClearCutOff() {
  shut_out_since_ms_ = 0;
  rejoin_asked_ms_ = 0;
  rejoin_asks_ = 0;
  together_given_up_ = false;
  disconnected_said_ = false;
}

bool ParticipantLivenessWatch::SetCutOff(const bool cut_off, ParticipantLivenessStep& step) {
  step.cut_off = cut_off;
  step.cut_off_changed = cut_off != cut_off_;
  cut_off_ = cut_off;
  return cut_off;
}

void ParticipantLivenessWatch::Pause(const int64_t now_ms, const bool live, ParticipantLivenessStep& step) {
  live_since_ms_ = live ? now_ms : 0;
  shut_out_since_ms_ = 0;
  SetCutOff(false, step); // our side's own recovery says so meanwhile
  SetQuiet({}, step);
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
  cut_off_ = false;
  ClearCutOff();
}

} // namespace pbr
