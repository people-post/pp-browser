#include "messaging/IdUtil.h"

#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace pbr {

std::string GenerateUuid() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<uint64_t> dist;
  const uint64_t a = dist(rng);
  const uint64_t b = dist(rng);
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << a << std::setw(16) << b;
  return out.str();
}

int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace pbr
