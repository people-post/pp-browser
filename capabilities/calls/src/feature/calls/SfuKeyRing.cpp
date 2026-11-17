#include "feature/calls/SfuKeyRing.h"

#include <algorithm>
#include <utility>

namespace pbr {

void SfuKeyRing::Install(const uint32_t epoch, ByteVector key, const int64_t now_ms,
                         const std::vector<uint32_t>& hold_for) {
  std::lock_guard lock(mutex_);
  if (key.empty() || (!keys_.empty() && epoch <= keys_.back().key.epoch)) {
    return;
  }
  Prune(now_ms);
  const bool first = keys_.empty();
  keys_.push_back(Held{Key{epoch, std::move(key)}});
  if (first) {
    sealing_epoch_ = epoch;
    return;
  }
  if (hold_for.empty()) {
    SealWith(epoch, now_ms);
    return;
  }
  // Ours: the old epoch seals on until the others hold this one (a newer rotation's hold replaces an older one's).
  hold_ = Hold{epoch, now_ms + kSealHoldMaxMs, std::set<uint32_t>(hold_for.begin(), hold_for.end())};
}

SfuKeyRing::Key SfuKeyRing::Current(const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  MaybeEndHold(now_ms);
  for (const Held& held : keys_) {
    if (held.key.epoch == sealing_epoch_) {
      return held.key;
    }
  }
  return {};
}

std::vector<SfuKeyRing::Key> SfuKeyRing::ForOpen(const int64_t now_ms) const {
  std::lock_guard lock(mutex_);
  std::vector<Key> keys;
  for (auto it = keys_.rbegin(); it != keys_.rend(); ++it) {
    if (now_ms < it->open_until_ms) {
      keys.push_back(it->key);
    }
  }
  return keys;
}

void SfuKeyRing::NoteOpened(const uint32_t stream_id, const uint32_t epoch, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  if (!hold_ || epoch < hold_->epoch) {
    return;
  }
  hold_->waiting.erase(stream_id);
  MaybeEndHold(now_ms);
}

void SfuKeyRing::MaybeEndHold(const int64_t now_ms) {
  if (hold_ && (hold_->waiting.empty() || now_ms >= hold_->until_ms)) {
    SealWith(hold_->epoch, now_ms);
  }
}

void SfuKeyRing::SealWith(const uint32_t epoch, const int64_t now_ms) {
  hold_.reset();
  sealing_epoch_ = epoch;
  for (Held& held : keys_) {
    if (held.key.epoch < epoch && held.open_until_ms == kOpen) {
      held.open_until_ms = now_ms + kPreviousEpochGraceMs;
    }
  }
  Prune(now_ms);
  while (keys_.size() > kKeysKept && keys_.front().key.epoch < sealing_epoch_) {
    keys_.erase(keys_.begin());
  }
}

void SfuKeyRing::Prune(const int64_t now_ms) {
  keys_.erase(
      std::remove_if(keys_.begin(), keys_.end(), [&](const Held& held) { return held.open_until_ms <= now_ms; }),
      keys_.end());
}

} // namespace pbr
