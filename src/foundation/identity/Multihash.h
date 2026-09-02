#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace pbr {

enum class HashType : uint64_t {
  kIdentity = 0x0,
  kSha256 = 0x12,
};

class Multihash {
 public:
  static constexpr uint8_t kMaxHashLength = 127;

  static Multihash Create(HashType type, std::span<const uint8_t> hash);

  HashType type() const { return type_; }
  std::span<const uint8_t> hash() const;
  const std::vector<uint8_t>& buffer() const { return bytes_; }

 private:
  Multihash(HashType type, std::span<const uint8_t> hash);

  HashType type_{};
  std::vector<uint8_t> bytes_;
  size_t hash_offset_{};
};

} // namespace pbr
