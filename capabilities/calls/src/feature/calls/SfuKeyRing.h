#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The media keys of one hop attach (V003: the key rotates on every leave). Frames are sealed with
 * the newest epoch; the previous one still opens frames for a short grace while the others switch
 * (a frame names no epoch — it is bound into the AAD). Thread-safe: frames seal and open on the
 * mesh's threads; a new epoch is installed from the calls owner.
 */
class SfuKeyRing {
public:
  struct Key {
    uint32_t epoch = 0;
    ByteVector key;
  };
  /** How long the previous epoch still opens frames after a newer one is installed. */
  static constexpr int64_t kPreviousEpochGraceMs = 3000;

  /** `epoch` becomes the newest; an older or the same epoch changes nothing. */
  void Install(uint32_t epoch, ByteVector key, int64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (key.empty() || (!current_.key.empty() && epoch <= current_.epoch)) {
      return;
    }
    if (!current_.key.empty()) {
      previous_ = current_;
      previous_until_ms_ = now_ms + kPreviousEpochGraceMs;
    }
    current_ = Key{epoch, std::move(key)};
  }

  /** The key to seal with (empty key: none yet). */
  Key Current() const {
    std::lock_guard lock(mutex_);
    return current_;
  }

  /** Keys to open a frame with, in order: the newest, then the previous within its grace. */
  std::vector<Key> ForOpen(int64_t now_ms) const {
    std::lock_guard lock(mutex_);
    std::vector<Key> keys;
    if (!current_.key.empty()) {
      keys.push_back(current_);
    }
    if (previous_ && now_ms < previous_until_ms_) {
      keys.push_back(*previous_);
    }
    return keys;
  }

private:
  mutable std::mutex mutex_;
  Key current_;
  std::optional<Key> previous_;
  int64_t previous_until_ms_ = 0;
};

} // namespace pbr
