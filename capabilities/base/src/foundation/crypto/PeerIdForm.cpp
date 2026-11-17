#include "foundation/crypto/PeerIdForm.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

namespace {

constexpr std::string_view kBase58Alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr size_t kMultihashBytes = 34;
constexpr size_t kPeerIdChars = 46;

/** Base58btc to bytes, leading `1`s as leading zero bytes; nullopt on a character outside the alphabet. */
std::optional<std::vector<uint8_t>> Base58Decode(std::string_view text) {
  std::vector<uint8_t> big; // big-endian, without the leading zeros
  for (const char c : text) {
    const size_t digit = kBase58Alphabet.find(c);
    if (digit == std::string_view::npos) {
      return std::nullopt;
    }
    uint32_t carry = static_cast<uint32_t>(digit);
    for (auto it = big.rbegin(); it != big.rend(); ++it) {
      carry += static_cast<uint32_t>(*it) * 58;
      *it = static_cast<uint8_t>(carry & 0xff);
      carry >>= 8;
    }
    while (carry > 0) {
      big.insert(big.begin(), static_cast<uint8_t>(carry & 0xff));
      carry >>= 8;
    }
  }
  size_t zeros = 0;
  while (zeros < text.size() && text[zeros] == '1') {
    ++zeros;
  }
  std::vector<uint8_t> out(zeros, 0);
  out.insert(out.end(), big.begin(), big.end());
  return out;
}

std::string Base58Encode(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> digits; // base 58, most significant first
  for (const uint8_t byte : bytes) {
    uint32_t carry = byte;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
      carry += static_cast<uint32_t>(*it) << 8;
      *it = static_cast<uint8_t>(carry % 58);
      carry /= 58;
    }
    while (carry > 0) {
      digits.insert(digits.begin(), static_cast<uint8_t>(carry % 58));
      carry /= 58;
    }
  }
  std::string out;
  for (size_t i = 0; i < bytes.size() && bytes[i] == 0; ++i) {
    out.push_back('1');
  }
  for (const uint8_t digit : digits) {
    out.push_back(kBase58Alphabet[digit]);
  }
  return out;
}

} // namespace

bool IsMlDsaPeerIdForm(const std::string_view text) {
  if (text.size() != kPeerIdChars) {
    return false;
  }
  const auto bytes = Base58Decode(text);
  if (!bytes || bytes->size() != kMultihashBytes || (*bytes)[0] != 0x12 || (*bytes)[1] != 0x20) {
    return false;
  }
  return Base58Encode(*bytes) == text;
}

} // namespace pbr
