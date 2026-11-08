#include "lib/messaging/ControlLaneHold.h"

#include <algorithm>
#include <utility>

namespace pbr {

bool ControlLaneHold::Hold(const std::string& sender, RelayEnvelope envelope, const int64_t held_since_ms,
                           const int64_t now_ms, ControlLaneHoldDrops* drops) {
  ControlLaneHoldDrops dropped;
  DropExpired(now_ms, dropped);
  const bool in_time = now_ms - held_since_ms <= kControlLaneHoldMaxMs;
  const bool held_already = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& entry) {
    return entry.sender == sender && entry.held.envelope.message_id == envelope.message_id;
  });
  if (in_time && !held_already) {
    if (SizeOf(sender) >= kControlLaneHoldPerSender) {
      DropOldest(&sender);
      ++dropped.over_sender;
    }
    if (entries_.size() >= kControlLaneHoldTotal) {
      DropOldest(nullptr);
      ++dropped.over_total;
    }
    entries_.push_back(Entry{sender, HeldControlEnvelope{std::move(envelope), held_since_ms}});
  }
  if (drops) {
    *drops = dropped;
  }
  return in_time;
}

std::vector<HeldControlEnvelope> ControlLaneHold::Take(const std::string& sender, const int64_t now_ms,
                                                       ControlLaneHoldDrops* drops) {
  ControlLaneHoldDrops dropped;
  DropExpired(now_ms, dropped);
  std::vector<HeldControlEnvelope> taken;
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->sender == sender) {
      taken.push_back(std::move(it->held));
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  std::stable_sort(taken.begin(), taken.end(), [](const HeldControlEnvelope& a, const HeldControlEnvelope& b) {
    return a.held_since_ms < b.held_since_ms;
  });
  if (drops) {
    *drops = dropped;
  }
  return taken;
}

bool ControlLaneHold::Holds(const std::string& sender) const {
  return std::any_of(entries_.begin(), entries_.end(),
                     [&sender](const Entry& entry) { return entry.sender == sender; });
}

size_t ControlLaneHold::SizeOf(const std::string& sender) const {
  return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(),
                                           [&sender](const Entry& entry) { return entry.sender == sender; }));
}

void ControlLaneHold::DropExpired(const int64_t now_ms, ControlLaneHoldDrops& drops) {
  const auto before = entries_.size();
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [now_ms](const Entry& entry) {
                                  return now_ms - entry.held.held_since_ms > kControlLaneHoldMaxMs;
                                }),
                 entries_.end());
  drops.expired += before - entries_.size();
}

void ControlLaneHold::DropOldest(const std::string* sender) {
  auto oldest = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (sender && it->sender != *sender) {
      continue;
    }
    if (oldest == entries_.end() || it->held.held_since_ms < oldest->held.held_since_ms) {
      oldest = it;
    }
  }
  if (oldest != entries_.end()) {
    entries_.erase(oldest);
  }
}

RelayEnvelope AgedByHold(RelayEnvelope envelope, const int64_t held_since_ms, const int64_t now_ms) {
  const int64_t waited = std::max<int64_t>(0, now_ms - held_since_ms);
  if (envelope.relay_created_at_ms && envelope.relay_server_time_ms) {
    *envelope.relay_server_time_ms += waited;
  } else {
    // Through a link: no relay time of its own; the wait here is how long it was on its way.
    envelope.relay_created_at_ms = held_since_ms;
    envelope.relay_server_time_ms = held_since_ms + waited;
  }
  return envelope;
}

} // namespace pbr
