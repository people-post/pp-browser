#include "foundation/crypto/PinKeyDeriver.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

PinKdfParams PinKeyDeriver::DefaultParams() {
  PinKdfParams params;
  params.opslimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
  params.memlimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;
  params.salt.assign(crypto_pwhash_SALTBYTES, 0);
  return params;
}

Roe<PinKdfParams> PinKeyDeriver::GenerateParams() {
  EnsureSodiumInit();
  PinKdfParams params = DefaultParams();
  randombytes_buf(params.salt.data(), params.salt.size());
  return params;
}

Roe<ByteVector> PinKeyDeriver::DeriveKek(std::string_view pin, const PinKdfParams& params) {
  if (pin.empty()) {
    return Error("PIN must not be empty");
  }
  if (params.salt.size() != crypto_pwhash_SALTBYTES) {
    return Error("Invalid PIN KDF salt size");
  }
  if (params.opslimit == 0 || params.memlimit == 0) {
    return Error("Invalid PIN KDF limits");
  }
  EnsureSodiumInit();
  ByteVector kek(kDataEncryptionKeySize);
  if (crypto_pwhash(kek.data(), kek.size(), pin.data(), pin.size(), params.salt.data(), params.opslimit,
                    params.memlimit, crypto_pwhash_ALG_ARGON2ID13) != 0) {
    return Error("PIN key derivation failed");
  }
  return kek;
}

} // namespace pbr
