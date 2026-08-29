/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/peer/peer_id.hpp>

#include <cassert>
#include <libp2p/crypto/sha/sha256.hpp>
#include <libp2p/multi/multibase_codec/codecs/base58.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::peer, PeerId::FactoryError, e) {
  using E = libp2p::peer::PeerId::FactoryError;
  switch (e) {
    case E::SUCCESS:
      return "success";
    case E::SHA256_EXPECTED:
      return "expected a sha-256 multihash";
  }
  return "unknown error";
}

namespace libp2p::peer {
  using multi::Multihash;
  using multi::detail::decodeBase58;
  using multi::detail::encodeBase58;

  PeerId::PeerId(const multi::Multihash &hash) : hash_{hash} {}

  PeerId::FactoryResult PeerId::fromPublicKey(const crypto::ProtobufKey &key) {
    std::vector<uint8_t> hash;

    auto algo = multi::sha256;
    if (key.key.size() <= kMaxInlineKeyLength) {
      algo = multi::identity;
      hash = key.key;
    } else {
      auto shash_res = crypto::sha256(key.key);
      if (!shash_res) {
        return std::move(shash_res).as_failure();
      }
      auto shash = std::move(shash_res).value();
      hash = std::vector<uint8_t>{shash.begin(), shash.end()};
    }

    auto multihash_res = Multihash::create(algo, hash);
    if (!multihash_res) {
      return multihash_res.error();
    }
    // Copy from the lvalue outcome; avoid MSVC move-from-temporary pitfalls.
    return PeerId{multihash_res.value()};
  }

  PeerId::FactoryResult PeerId::fromBase58(std::string_view id) {
    auto decoded_id_res = decodeBase58(id);
    if (!decoded_id_res) {
      return std::move(decoded_id_res).as_failure();
    }
    auto decoded_id = std::move(decoded_id_res).value();

    auto hash_res = Multihash::createFromBytes(decoded_id);
    if (!hash_res) {
      return std::move(hash_res).as_failure();
    }
    const auto &hash = hash_res.value();

    if (hash.getType() != multi::HashType::sha256
        && hash.toBuffer().size() > kMaxInlineKeyLength) {
      return FactoryError::SHA256_EXPECTED;
    }

    return PeerId{hash};
  }

  PeerId::FactoryResult PeerId::fromHash(const Multihash &hash) {
    if (hash.getType() != multi::HashType::sha256
        && hash.toBuffer().size() > kMaxInlineKeyLength) {
      return FactoryError::SHA256_EXPECTED;
    }

    return PeerId{hash};
  }

  bool PeerId::operator<(const PeerId &other) const {
    return this->hash_ < other.hash_;
  }

  bool PeerId::operator==(const PeerId &other) const {
    return this->hash_ == other.hash_;
  }

  bool PeerId::operator!=(const PeerId &other) const {
    return !(*this == other);
  }

  std::string PeerId::toBase58() const {
    return encodeBase58(hash_.toBuffer());
  }

  libp2p::Bytes PeerId::toVector() const {
    return hash_.toBuffer();
  }

  std::string PeerId::toHex() const {
    return hash_.toHex();
  }

  const multi::Multihash &PeerId::toMultihash() const {
    return hash_;
  }

  PeerId::FactoryResult PeerId::fromBytes(BytesIn v) {
    auto mh_res = Multihash::createFromBytes(v);
    if (!mh_res) {
      return std::move(mh_res).as_failure();
    }
    auto mh = std::move(mh_res).value();
    return fromHash(mh);
  }
}  // namespace libp2p::peer

size_t std::hash<libp2p::peer::PeerId>::operator()(
    const libp2p::peer::PeerId &peer_id) const {
  return std::hash<libp2p::multi::Multihash>()(peer_id.toMultihash());
}
