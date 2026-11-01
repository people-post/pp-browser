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
  /** BLAKE2b-256 over exactly what a device decrypts in one copy: 0x02 ‖ LenBytes(payload). */
  static Roe<std::vector<uint8_t>> CopyHash(const RelayCopy& copy);
  /**
   * 1 to kMaxRelayCopies copies, one per device (at most one account copy); a group envelope names its group,
   * a direct / control one does not, and the control lane is one account copy. Anything else is malformed —
   * signing and verifying both refuse it.
   */
  static Roe<void> CheckBodyShape(const RelayEnvelope& envelope);
  static Roe<bool> Verify(const RelayEnvelope& envelope, const std::string& public_key_b64);

private:
  static uint8_t RouteKindByte(const RelayRoute& route);
  static uint8_t ChannelByte(const RelayRoute& route);
};

} // namespace pbr
