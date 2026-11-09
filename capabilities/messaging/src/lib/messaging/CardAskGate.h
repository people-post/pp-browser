#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * How far past this device's clock an envelope's `sender_card_at` may be and still count as a list version: no
 * card is issued later than its signer's clock, so a hint beyond this is forged or broken and counts for nothing.
 */
inline constexpr int64_t kCardAtClockSkewMs = 10 * 60 * 1000;
/** An account asked for its card is asked again no sooner than this (unless a verified envelope names a newer list). */
inline constexpr int64_t kCardReaskGapMs = 10 * 60 * 1000;
/** Asks on an unverified envelope's word, whoever it names: at most this many per kUnverifiedCardAskWindowMs. */
inline constexpr size_t kUnverifiedCardAsksPerWindow = 8;
inline constexpr int64_t kUnverifiedCardAskWindowMs = 60 * 1000;
/** Accounts remembered at most (asked within kCardReaskGapMs); past it, the longest-asked go first. */
inline constexpr size_t kCardAskAccountsMax = 512;

/**
 * Whether to ask an account's device for its card (`card_ask`, M021 d6 / M025). An envelope's `sender_card_at` is an
 * unsigned hint, so nothing it says may stop a later ask: asks are remembered by when they went, and an account is
 * asked again after kCardReaskGapMs whatever list version was claimed. Only a verified envelope's hint, no further
 * ahead than kCardAtClockSkewMs, counts as a version, and a newer one than last asked for asks at once. An
 * unverified envelope (no card to check it with) asks at most once per account per gap and, from all senders
 * together, at most kUnverifiedCardAsksPerWindow per window: anyone can forge one, naming any account. Bounded:
 * entries past the gap are forgotten (they decide nothing), and at most kCardAskAccountsMax are kept. Pure: the
 * caller locks, and gives the clock.
 */
class CardAskGate {
public:
  enum class Source {
    /** The envelope verified with a card kept: its hint said the list is newer than that card. */
    Verified,
    /** No card to check the envelope with (none kept, its device not on it), or a roster change held for one. */
    Unverified,
  };
  /** True when `account` is to be asked now (and the ask is counted); `claimed_at` is the hint (ms). */
  bool Allow(const std::string& account, int64_t claimed_at, Source source, int64_t now_ms);
  size_t Size() const { return entries_.size(); }

private:
  struct Entry {
    /** The newest list version asked for on a verified envelope's word (0: none). */
    int64_t asked_for = 0;
    int64_t asked_at_ms = 0;
  };
  void Forget(int64_t now_ms);
  bool TakeUnverifiedSlot(int64_t now_ms);

  std::map<std::string, Entry> entries_;
  std::deque<int64_t> unverified_asks_ms_; // within the window, oldest first
};

} // namespace pbr
