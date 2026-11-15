#include "feature/presenters/CallSilentTiles.h"

#include "common/media/MediaRelayStream.h"

#include <algorithm>
#include <set>
#include <utility>

namespace pbr {

CallSilentTiles::CallSilentTiles(LogFn log, CallSilentTilesOptions options) : log_(std::move(log)), options_(options) {}

bool CallSilentTiles::BeginRefresh(const std::string& call_id, const bool roster_read, const bool group,
                                   const int64_t now_ms) {
  if (call_id != call_id_) {
    Reset();
    call_id_ = call_id;
  }
  if (!roster_read) {
    return false; // no list this refresh: nobody is forgotten
  }
  was_group_ = was_group_ || group;
  if (!was_group_) {
    return false;
  }
  return last_step_ms_ == 0 || now_ms - last_step_ms_ >= kCounterReadMs;
}

void CallSilentTiles::Reset() {
  was_group_ = false;
  people_.clear();
  own_baseline_ms_ = 0;
  last_step_ms_ = 0;
  was_online_ = false;
}

const char* CallSilentTiles::BaselineName(const Baseline b) {
  switch (b) {
    case Baseline::Packet:
      return "packet";
    case Baseline::Join:
      return "join";
    case Baseline::Own:
      return "own";
  }
  return "packet";
}

const char* CallSilentTiles::BucketName(const int64_t silent_ms) {
  if (silent_ms < kSilentTileMs) {
    return "<15s";
  }
  if (silent_ms < 20'000) {
    return "15-20s";
  }
  if (silent_ms < 60'000) {
    return "20-60s";
  }
  return "60s+";
}

CallSilentTiles::Silence CallSilentTiles::SilenceOf(const State& s, const int64_t now_ms) const {
  // The latest start wins; a tie reads as the packet, then the join.
  Silence out;
  int64_t start = s.last_progress_ms;
  out.baseline = Baseline::Packet;
  if (s.join_baseline_ms > start) {
    start = s.join_baseline_ms;
    out.baseline = Baseline::Join;
  }
  if (own_baseline_ms_ > start) {
    start = own_baseline_ms_;
    out.baseline = Baseline::Own;
  }
  out.ms = now_ms - start;
  return out;
}

void CallSilentTiles::Emit(const char* what, const Silence& silence) const {
  if (!log_) {
    return;
  }
  // Only the bucket and which baseline applied: no identity, name, address or stream number.
  log_(std::string("call tile ") + what + " silent=" + BucketName(silence.ms) +
       " baseline=" + BaselineName(silence.baseline));
}

CallSilentTiles::Tile CallSilentTiles::TileOf(const std::string& identity) const {
  Tile tile;
  const auto it = people_.find(identity);
  if (it == people_.end()) {
    return tile;
  }
  const State& s = it->second;
  tile.silent = s.dimmed && !s.on_hold;
  tile.video_blocked = s.dimmed || s.awaiting_video;
  return tile;
}

void CallSilentTiles::Step(const int64_t now_ms, const bool own_online, const std::vector<Person>& others) {
  // Our own path coming back, or a tick gap (the UI was suspended): the silence restarts from now.
  if ((own_online && !was_online_) || (last_step_ms_ != 0 && now_ms - last_step_ms_ > kTickGapResetMs)) {
    own_baseline_ms_ = now_ms;
  }
  was_online_ = own_online;
  last_step_ms_ = now_ms;

  // Whoever is no longer in the list is forgotten, so a return starts from a fresh join.
  std::set<std::string> present;
  for (const Person& p : others) {
    present.insert(p.identity);
  }
  for (auto it = people_.begin(); it != people_.end();) {
    it = present.count(it->first) ? std::next(it) : people_.erase(it);
  }

  for (const Person& p : others) {
    auto [it, fresh] = people_.try_emplace(p.identity);
    State& s = it->second;
    if (fresh) {
      s.join_baseline_ms = now_ms; // the first readings are only the references
    }
    if (s.on_hold && !p.on_hold) {
      s.join_baseline_ms = now_ms; // back from hold: their media resumes only now
    }
    s.on_hold = p.on_hold;
    const bool audio_up = s.audio_ref && p.audio > *s.audio_ref;
    const bool video_up = s.video_ref && p.video > *s.video_ref;
    s.audio_ref = p.audio; // an increase, no change or a decrease: the reference follows the reading
    s.video_ref = p.video;
    if (video_up) {
      s.awaiting_video = false;
    }
    if (!(audio_up || video_up)) {
      continue;
    }
    if (s.dimmed) {
      Emit("restored", SilenceOf(s, now_ms));
      s.dimmed = false;
    }
    s.ever_received = true;
    s.last_progress_ms = now_ms;
  }

  // Fallback: everyone we have received from went quiet together, so it is our own path, not theirs.
  bool all_stopped = false;
  if (options_.all_stop_fallback) {
    int counted = 0;
    bool all_silent = true;
    for (const auto& [identity, s] : people_) {
      if (s.on_hold || !s.ever_received) {
        continue;
      }
      ++counted;
      all_silent = all_silent && SilenceOf(s, now_ms).ms >= kSilentTileMs;
    }
    all_stopped = counted >= 2 && all_silent;
  }

  if (!own_online || all_stopped) {
    return; // nobody new is dimmed; the ones already dimmed stay
  }
  for (auto& [identity, s] : people_) {
    if (s.dimmed || !s.ever_received || s.on_hold) {
      continue;
    }
    const Silence silence = SilenceOf(s, now_ms);
    if (silence.ms >= kSilentTileMs) {
      s.dimmed = true;
      s.awaiting_video = true;
      Emit("dimmed", silence);
    }
  }
}

std::vector<CallSilentTiles::Person> SilentPeopleFrom(const std::vector<CallScreen::Member>& members,
                                                      const MediaEngineHealth& health) {
  std::vector<CallSilentTiles::Person> others;
  for (const CallScreen::Member& m : members) {
    if (m.is_local) {
      continue;
    }
    CallSilentTiles::Person p{.identity = m.identity, .on_hold = m.on_hold};
    const uint32_t stream = PublisherStreamIdForIdentity(p.identity);
    for (const MediaStreamHealth& audio : health.streams) {
      if (audio.stream_id == stream) {
        p.audio = audio.rx_frames;
        break;
      }
    }
    for (const VideoStreamArrival& video : health.video_arrivals) {
      if (video.stream_id == stream) {
        p.video = video.arrival.frames;
        break;
      }
    }
    others.push_back(std::move(p));
  }
  return others;
}

} // namespace pbr
