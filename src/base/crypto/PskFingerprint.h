#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <string>

namespace pbr {

/** BLAKE2b-256 fingerprint display (E011). */
class PskFingerprint {
public:
  static Roe<ByteVector> Compute(const ByteVector& master_psk);
  static std::string FormatDisplay(const ByteVector& digest);
  static Roe<ByteVector> ParseDisplay(std::string_view grouped_hex);
};

} // namespace pbr
