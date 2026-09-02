#pragma once

#include "base/mesh/identity/Multihash.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace pbr {

class PeerId {
 public:
  static constexpr size_t kMaxInlineKeyLength = 42;

  static PeerId FromProtobufPublicKey(std::span<const uint8_t> protobuf_key);

  std::string ToBase58() const;
  const Multihash& multihash() const { return hash_; }

 private:
  explicit PeerId(Multihash hash);

  Multihash hash_;
};

} // namespace pbr
