#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class SessionKeyDeriver {
public:
  static Roe<ByteVector> Derive(const ByteVector& master_psk, CryptoChannel channel, uint32_t session_epoch);
  static std::string BuildHkdfInfo(CryptoChannel channel, uint32_t session_epoch);
};

} // namespace pbr
