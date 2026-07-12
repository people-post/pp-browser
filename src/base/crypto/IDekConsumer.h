#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

namespace pbr {

/**
 * Profile DEK consumer — MessagingHub fans out unlock/lock via this interface.
 * New at-rest stores implement SetDek/ClearDek and register with the hub.
 */
class IDekConsumer {
public:
  virtual ~IDekConsumer() = default;

  /** Install a 32-byte DEK copy; replace any previous key (zeroed). */
  virtual Roe<void> SetDek(ByteVector dek) = 0;

  /** Zero and drop the in-memory DEK (vault lock / shutdown). */
  virtual void ClearDek() = 0;
};

} // namespace pbr
