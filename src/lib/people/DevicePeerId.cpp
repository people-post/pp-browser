#include "lib/people/DevicePeerId.h"

#include "crypto/PeerId.h"
#include "foundation/crypto/CryptoUtil.h"

namespace pbr {

Roe<std::string> DevicePeerIdFromKey(const std::string& device_public_key_b64) {
  auto key = Base64Decode(device_public_key_b64);
  if (!key) {
    return key.error();
  }
  return pp::PeerIdFromMlDsaPublicKey(*key);
}

} // namespace pbr
