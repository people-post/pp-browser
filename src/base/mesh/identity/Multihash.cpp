#include "base/mesh/identity/Multihash.h"

#include <cassert>

namespace pbr {

namespace {

void AppendVarint(std::vector<uint8_t>& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

} // namespace

Multihash Multihash::Create(HashType type, std::span<const uint8_t> hash) {
  return Multihash(type, hash);
}

Multihash::Multihash(HashType type, std::span<const uint8_t> hash) : type_(type) {
  bytes_.reserve(hash.size() + 4);
  AppendVarint(bytes_, static_cast<uint64_t>(type));
  assert(hash.size() <= std::numeric_limits<uint8_t>::max());
  bytes_.push_back(static_cast<uint8_t>(hash.size()));
  hash_offset_ = bytes_.size();
  bytes_.insert(bytes_.end(), hash.begin(), hash.end());
}

std::span<const uint8_t> Multihash::hash() const {
  return std::span<const uint8_t>(bytes_).subspan(hash_offset_);
}

} // namespace pbr
