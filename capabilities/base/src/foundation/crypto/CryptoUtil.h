#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "crypto/SodiumUtil.h"

#include "common/PbrCompat.h"

namespace pbr {

// pp-cpp-crypto's libsodium helpers, spelled in pbr (capability-repos P2: the base keeps no copy).
using ::pp::Base64Decode;
using ::pp::Base64Encode;
using ::pp::Base64UrlDecode;
using ::pp::Base64UrlEncode;
using ::pp::Blake2b256;
using ::pp::BytesToHex;
using ::pp::EnsureSodiumInit;
using ::pp::HexToBytes;

} // namespace pbr
