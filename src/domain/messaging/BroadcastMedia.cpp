#include "domain/messaging/BroadcastMedia.h"

#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>

namespace pbr {

std::vector<uint8_t> NewBroadcastMediaKey() {
  EnsureSodiumInit();
  std::vector<uint8_t> key(32);
  randombytes_buf(key.data(), key.size());
  return key;
}

std::string NewBroadcastJoinHandle(const std::string& program_id) {
  EnsureSodiumInit();
  std::vector<uint8_t> rnd(8);
  randombytes_buf(rnd.data(), rnd.size());
  return "live:" + program_id + ":" + BytesToHex(rnd);
}

} // namespace pbr
