#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

struct PinKdfParams {
  uint64_t opslimit = 0;
  uint64_t memlimit = 0;
  ByteVector salt;
};

class PinKeyDeriver {
public:
  static PinKdfParams DefaultParams();
  static Roe<PinKdfParams> GenerateParams();
  static Roe<ByteVector> DeriveKek(std::string_view pin, const PinKdfParams& params);
};

} // namespace pbr
