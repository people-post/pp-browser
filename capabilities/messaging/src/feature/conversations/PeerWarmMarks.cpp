#include "feature/conversations/PeerWarmMarks.h"

#include <utility>
#include <vector>

namespace pbr {

void PeerWarmMarks::Bind(PeerFn mark, PeerFn clear) {
  std::lock_guard lock(mu_);
  mark_ = std::move(mark);
  clear_ = std::move(clear);
}

void PeerWarmMarks::Reset() {
  std::lock_guard lock(mu_);
  mark_ = nullptr;
  clear_ = nullptr;
  warmth_.Clear();
}

void PeerWarmMarks::Warm(const std::string& peer_key, const int64_t now_ms) {
  PeerFn mark;
  {
    std::lock_guard lock(mu_);
    if (peer_key.empty()) {
      return;
    }
    (void)warmth_.Warm(peer_key, now_ms);
    mark = mark_;
  }
  if (mark) {
    mark(peer_key); // every time: the link may be a new one since the last mark
  }
}

void PeerWarmMarks::Touch(const std::string& peer_key, const int64_t now_ms) {
  std::lock_guard lock(mu_);
  (void)warmth_.Touch(peer_key, now_ms);
}

void PeerWarmMarks::Tick(const int64_t now_ms) {
  std::vector<std::string> lapsed;
  PeerFn clear;
  {
    std::lock_guard lock(mu_);
    lapsed = warmth_.Lapsed(now_ms);
    clear = clear_;
  }
  if (!clear) {
    return;
  }
  for (const std::string& peer_key : lapsed) {
    clear(peer_key);
  }
}

bool PeerWarmMarks::IsWarm(const std::string& peer_key) const {
  std::lock_guard lock(mu_);
  return warmth_.IsWarm(peer_key);
}

} // namespace pbr
