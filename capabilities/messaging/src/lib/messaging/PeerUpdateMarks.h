#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/** A peer marked as needing a newer app here is told about again no sooner than this (one notice, not a flood). */
inline constexpr int64_t kNeedsUpdateNoticeGapMs = 24 * 60 * 60 * 1000;
/** Peers remembered at most; past it, the longest unheard go first. */
inline constexpr size_t kPeerUpdateMarksMax = 512;

/**
 * Who has sent this device something only a newer app reads (ReceiveFailure::NeedsUpdate, INVENTORY U1): a message
 * kept as "update to view", or an account card of a newer version (U2). The chat with such a peer says so in its
 * header, and the user is told once per peer per kNeedsUpdateNoticeGapMs. In memory: the next such message marks
 * the peer again after a restart (its stored "update to view" rows stay). Pure: the caller locks, and gives the
 * clock.
 */
class PeerUpdateMarks {
public:
  /** Marks `account` (seen now); true when the user is to be told now (the first time, or the gap is over). */
  bool Note(const std::string& account, int64_t now_ms);
  /** `account` was marked: something it sent needs a newer app here. */
  bool Marked(const std::string& account) const { return marks_.count(account) > 0; }
  size_t Size() const { return marks_.size(); }

private:
  struct Mark {
    int64_t seen_at_ms = 0;
    int64_t told_at_ms = 0;
  };
  std::map<std::string, Mark> marks_;
};

} // namespace pbr
