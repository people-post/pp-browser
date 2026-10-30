#include "foundation/crypto/RecoveryPhrase.h"

#include "foundation/crypto/CryptoUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sodium.h>
#include <sstream>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr std::array<std::string_view, 2048> kWords = {
#include "foundation/crypto/Bip39EnglishWords.inc"
};

constexpr size_t kBitsPerWord = 11;

uint8_t ChecksumByte(const ByteVector& seed) {
  EnsureSodiumInit();
  unsigned char digest[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(digest, seed.data(), seed.size());
  return digest[0];
}

/** Bit `i` (most significant first) of `bytes`. */
bool BitAt(const std::vector<uint8_t>& bytes, size_t i) { return (bytes[i / 8] >> (7 - i % 8)) & 1u; }

} // namespace

Roe<std::string> EncodeRecoveryPhrase(const ByteVector& seed) {
  if (seed.size() != kRecoverySeedBytes) {
    return Error("recovery seed must be 32 bytes");
  }
  std::vector<uint8_t> bits(seed.begin(), seed.end());
  bits.push_back(ChecksumByte(seed));  // 256 + 8 bits = 24 × 11
  std::string phrase;
  for (size_t w = 0; w < kRecoveryPhraseWords; ++w) {
    size_t index = 0;
    for (size_t b = 0; b < kBitsPerWord; ++b) {
      index = (index << 1) | (BitAt(bits, w * kBitsPerWord + b) ? 1u : 0u);
    }
    if (!phrase.empty()) {
      phrase += ' ';
    }
    phrase += kWords[index];
  }
  return phrase;
}

Roe<ByteVector> DecodeRecoveryPhrase(const std::string& phrase) {
  std::istringstream in(phrase);
  std::vector<std::string> words;
  for (std::string word; in >> word;) {
    std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return std::tolower(c); });
    words.push_back(std::move(word));
  }
  if (words.size() != kRecoveryPhraseWords) {
    return Error("a recovery phrase has 24 words (got " + std::to_string(words.size()) + ")");
  }
  std::vector<uint8_t> bits(kRecoverySeedBytes + 1, 0);
  for (size_t w = 0; w < words.size(); ++w) {
    const auto found = std::lower_bound(kWords.begin(), kWords.end(), std::string_view(words[w]));
    if (found == kWords.end() || *found != words[w]) {
      return Error("word " + std::to_string(w + 1) + " is not a recovery word");
    }
    const size_t index = static_cast<size_t>(found - kWords.begin());
    for (size_t b = 0; b < kBitsPerWord; ++b) {
      if ((index >> (kBitsPerWord - 1 - b)) & 1u) {
        const size_t bit = w * kBitsPerWord + b;
        bits[bit / 8] |= static_cast<uint8_t>(1u << (7 - bit % 8));
      }
    }
  }
  ByteVector seed(bits.begin(), bits.begin() + kRecoverySeedBytes);
  if (ChecksumByte(seed) != bits[kRecoverySeedBytes]) {
    return Error("the recovery phrase does not check out (a word is mistyped or out of order)");
  }
  return seed;
}

} // namespace pbr
