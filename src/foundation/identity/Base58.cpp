#include "foundation/identity/Base58.h"

#include <array>
#include <cstring>
#include <vector>

namespace pbr {

namespace {

constexpr std::string_view kBase58Alphabet =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

} // namespace

std::string EncodeBase58(std::span<const uint8_t> bytes) {
  int zeroes = 0;
  while (zeroes < static_cast<int>(bytes.size()) && bytes[static_cast<size_t>(zeroes)] == 0) {
    ++zeroes;
  }

  const auto* begin = bytes.data() + zeroes;
  const auto* end = bytes.data() + bytes.size();
  const int size = static_cast<int>((end - begin) * 138 / 100 + 1);
  std::vector<unsigned char> encoded(static_cast<size_t>(size), 0);
  int length = 0;

  for (const auto* p = begin; p != end; ++p) {
    int carry = *p;
    int i = 0;
    for (auto it = encoded.rbegin(); (carry != 0 || i < length) && it != encoded.rend(); ++it, ++i) {
      carry += 256 * (*it);
      *it = static_cast<unsigned char>(carry % 58);
      carry /= 58;
    }
    length = i;
  }

  auto it = encoded.begin() + (size - length);
  while (it != encoded.end() && *it == 0) {
    ++it;
  }

  std::string out;
  out.reserve(static_cast<size_t>(zeroes) + static_cast<size_t>(encoded.end() - it));
  out.assign(static_cast<size_t>(zeroes), '1');
  while (it != encoded.end()) {
    out.push_back(kBase58Alphabet[*it]);
    ++it;
  }
  return out;
}

} // namespace pbr
