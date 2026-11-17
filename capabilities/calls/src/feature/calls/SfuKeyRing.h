#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The media keys of one hop attach (V003: the key rotates on every leave; V068: when the switch happens). A frame
 * names no epoch — it is bound into the AAD — so a frame opens under each key held in turn. Thread-safe: frames seal
 * and open on the mesh's threads; a new epoch is installed from the calls owner.
 *
 * Sealing: a key taken from a peer seals at once. The rotation's minter keeps sealing with the old epoch until each
 * remaining participant has been seen sealing with the new one (a frame of theirs opened under it: they hold it), or
 * kSealHoldMaxMs passed — so nobody waits on a key still on its way to them.
 * Opening: every epoch from the one sealed with on; an older one for kPreviousEpochGraceMs after it stopped sealing
 * (the others switch on their own receipt of the key).
 */
class SfuKeyRing {
public:
  struct Key {
    uint32_t epoch = 0;
    ByteVector key;
  };
  /** How long an epoch this ring no longer seals with still opens frames (the others switch when the key reaches them). */
  static constexpr int64_t kPreviousEpochGraceMs = 3000;
  /** The longest the minter of a rotation keeps sealing with the old epoch while the others take the new key in. */
  static constexpr int64_t kSealHoldMaxMs = 3000;

  /**
   * `epoch` becomes the newest; an older or the same epoch changes nothing. `hold_for`: the streams of the others who
   * must be seen sealing with it before this ring does (this device minted it); empty: it seals at once.
   */
  void Install(uint32_t epoch, ByteVector key, int64_t now_ms, const std::vector<uint32_t>& hold_for = {});

  /** The key to seal with now (empty key: none yet). */
  Key Current(int64_t now_ms);

  /** Keys to open a frame with, in order: the newest first. */
  std::vector<Key> ForOpen(int64_t now_ms) const;

  /** A frame of `stream_id` opened under `epoch`: its sender holds that key. */
  void NoteOpened(uint32_t stream_id, uint32_t epoch, int64_t now_ms);

private:
  static constexpr int64_t kOpen = std::numeric_limits<int64_t>::max();
  /** Epochs kept at once (the one sealed with, those newer, and the ones in their grace). */
  static constexpr size_t kKeysKept = 4;
  struct Held {
    Key key;
    /** Until when it opens frames: kOpen while it is sealed with or newer. */
    int64_t open_until_ms = kOpen;
  };
  struct Hold {
    uint32_t epoch = 0;
    int64_t until_ms = 0;
    std::set<uint32_t> waiting;
  };
  /** Requires `mutex_`. Seal with `epoch` from now; the older ones open for their grace. */
  void SealWith(uint32_t epoch, int64_t now_ms);
  /** Requires `mutex_`. Drop the keys past their grace. */
  void Prune(int64_t now_ms);
  /** Requires `mutex_`. The hold ends when everyone waited on holds the key, or at its bound. */
  void MaybeEndHold(int64_t now_ms);

  mutable std::mutex mutex_;
  /** Ascending by epoch. */
  std::vector<Held> keys_;
  uint32_t sealing_epoch_ = 0;
  std::optional<Hold> hold_;
};

} // namespace pbr
