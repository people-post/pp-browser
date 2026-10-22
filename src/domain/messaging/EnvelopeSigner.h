#pragma once

#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** E014 canonical ML-DSA-65 relay envelope signing bytes (base/messaging). */
class EnvelopeSigner {
public:
  static Roe<std::vector<uint8_t>> BuildSignBytes(const RelayEnvelope& envelope);
  /** BLAKE2b-256 over exactly what the receiver decrypts (route-aware; see MESSAGE_ENCRYPTION.md). */
  static Roe<std::vector<uint8_t>> BodyHash(const RelayEnvelope& envelope);
  /**
   * Direct / control envelopes carry `payload_b64` only; group envelopes `member_payloads` only
   * (and a group_id). Anything else is malformed — signing and verifying both refuse it.
   */
  static Roe<void> CheckBodyShape(const RelayEnvelope& envelope);
  static Roe<bool> Verify(const RelayEnvelope& envelope, const std::string& public_key_b64);

private:
  static uint8_t RouteKindByte(const RelayRoute& route);
  static uint8_t ChannelByte(const RelayRoute& route);
};

} // namespace pbr
