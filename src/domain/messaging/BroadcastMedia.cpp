#include "domain/messaging/BroadcastMedia.h"

#include <algorithm>

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

int ChooseWatchVideoLevel(const std::vector<int>& published, const int preferred) {
  int below = 0;
  int above = 0;
  for (const int level : published) {
    if (level <= 0) {
      continue;
    }
    if (level <= preferred) {
      below = std::max(below, level);
    } else if (above == 0 || level < above) {
      above = level;
    }
  }
  return below != 0 ? below : above;
}

} // namespace pbr
