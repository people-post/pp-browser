#pragma once

#include "foundation/crypto/LinkHandshake.h"

#include "common/Error.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The new device's side of the link handshake while it waits (multi-device M027): its one-time KEM key, held in memory
 * only — never written, never sent — until the reply opened and the account came over (Forget), the person left the
 * link page (Forget), or its 15 minutes passed (wiped on the next call). A new offer replaces the one before.
 * Thread-safe.
 */
class LinkOffer {
public:
  /** What the new device shows: the QR code's bytes, the same as text to copy, and the 4 digits to compare. */
  struct Shown {
    ByteVector offer;
    std::string text;
    std::string code;
    int64_t expires_at_ms = 0;
  };

  /** A fresh one-time key (the one before is wiped), shown for kLinkWindowMs from `now_ms`. */
  Roe<Shown> Begin(int64_t now_ms);
  /** The offer being shown, while its window lasts (an expired one is wiped: nullopt). */
  std::optional<Shown> Current(int64_t now_ms);
  /**
   * Open the main device's reply (OpenLinkReply's errors; LinkError::Expired once the key is gone or its window
   * passed). The key stays until Forget: a reply that does not open can be followed by the right one.
   */
  Roe<LinkBundle> Open(std::span<const uint8_t> reply, int64_t now_ms);
  /** Wipe the key: the link is done or abandoned. */
  void Forget();

private:
  void ForgetLocked();
  /** The key and offer, or nothing when they are gone or out of time (wiped then). */
  bool LiveLocked(int64_t now_ms);

  std::mutex mutex_;
  LinkOfferKeys keys_;
  ByteVector offer_;
  int64_t expires_at_ms_ = 0;
};

} // namespace pbr
