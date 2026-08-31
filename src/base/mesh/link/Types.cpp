#include "base/mesh/link/Types.h"

#include <sodium.h>

namespace pbr::amp {

adp::PeerKey PreSessionPeerKey() {
  adp::PeerKey key{};
  static const char kLabel[] = "pp-amp-msh-presession-v1";
  crypto_generichash(key.bytes.data(), key.bytes.size(), reinterpret_cast<const unsigned char*>(kLabel),
                     sizeof(kLabel) - 1, nullptr, 0);
  return key;
}

adp::AssocId PreSessionAssocId() {
  adp::AssocId id{};
  id.bytes[0] = 0x01;
  return id;
}

} // namespace pbr::amp
