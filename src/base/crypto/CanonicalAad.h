#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

namespace pbr {

class CanonicalAad {
public:
  static Roe<ByteVector> Build(const AadFields& fields);
  static Roe<AadFields> Parse(const ByteVector& bytes);
};

} // namespace pbr
