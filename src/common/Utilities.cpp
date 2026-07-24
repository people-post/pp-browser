#include "common/Utilities.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace pbr::util {

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

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

std::string ToLowerAscii(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

} // namespace pbr::util
