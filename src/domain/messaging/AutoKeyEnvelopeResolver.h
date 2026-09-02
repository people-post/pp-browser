#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "common/chat/RelayEnvelope.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

/** Resolves stored PSK or derives from envelope key_init (E024). Lives in messaging — uses RelayEnvelope. */
Roe<ByteVector> ResolveOrDeriveMasterPsk(const RelayEnvelope& envelope, const ChatTargetKey& target_key,
                                         IPskSessionStore& psk_store, const ByteVector& local_private_key);

} // namespace pbr
