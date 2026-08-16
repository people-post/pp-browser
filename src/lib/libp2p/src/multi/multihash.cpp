/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/multi/multihash.hpp>

#include <boost/assert.hpp>
#include <boost/container_hash/hash.hpp>
#include <libp2p/basic/varint_prefix_reader.hpp>
#include <libp2p/common/types.hpp>
#include <qtils/hex.hpp>
#include <qtils/unhex.hpp>

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::multi, Multihash::Error, e) {
  using E = libp2p::multi::Multihash::Error;
  switch (e) {
    case E::ZERO_INPUT_LENGTH:
      return "The length encoded in the header is zero";
    case E::INCONSISTENT_LENGTH:
      return "The length encoded in the input data header doesn't match the "
             "actual length of the input data";
    case E::INPUT_TOO_LONG:
      return "The length of the input exceeds the maximum length of "
           + std::to_string(libp2p::multi::Multihash::kMaxHashLength);
    case E::INPUT_TOO_SHORT:
      return "The length of the input is less than the required minimum of two "
             "bytes for the multihash header";
    default:
      return "Unknown error";
  }
}

namespace libp2p::multi {

  Multihash::Multihash(HashType type, BytesIn hash) : data_(type, hash) {}

  namespace {
    template <typename Buffer>
    inline void appendVarint(Buffer &buffer, uint64_t t) {
      do {
        uint8_t byte = t & 0x7F;
        t >>= 7;
        if (t != 0) {
          byte |= 0x80;
        }
        buffer.push_back(byte);
      } while (t > 0);
    }
  }  // namespace

  Multihash::Data::Data(HashType t, BytesIn h) : type(t) {
    bytes.reserve(h.size() + 4);
    appendVarint(bytes, type);
    BOOST_ASSERT(h.size() <= std::numeric_limits<uint8_t>::max());
    bytes.push_back(static_cast<uint8_t>(h.size()));
    hash_offset = bytes.size();
    bytes.insert(bytes.end(), h.begin(), h.end());
    std_hash = boost::hash_range(bytes.begin(), bytes.end());
  }

  size_t Multihash::stdHash() const {
    return data_.std_hash;
  }

  outcome::result<Multihash> Multihash::create(HashType type, BytesIn hash) {
    if (hash.size() > kMaxHashLength) {
      return Error::INPUT_TOO_LONG;
    }

    return Multihash{type, hash};
  }

  outcome::result<Multihash> Multihash::createFromHex(std::string_view hex) {
    auto buf_res = qtils::unhex(hex);
    if (!buf_res) {
      return std::move(buf_res).as_failure();
    }
    auto buf = std::move(buf_res).value();
    return Multihash::createFromBytes(buf);
  }

  outcome::result<Multihash> Multihash::createFromBytes(BytesIn b) {
    if (b.size() < kHeaderSize) {
      return Error::INPUT_TOO_SHORT;
    }

    basic::VarintPrefixReader vr;
    if (vr.consume(b) != basic::VarintPrefixReader::kReady) {
      return Error::INPUT_TOO_SHORT;
    }

    const auto type = static_cast<HashType>(vr.value());
    if (b.empty()) {
      return Error::INPUT_TOO_SHORT;
    }

    const uint8_t length = b[0];
    BytesIn hash = b.subspan(1);

    if (length == 0) {
      return Error::ZERO_INPUT_LENGTH;
    }

    if (hash.size() != length) {
      return Error::INCONSISTENT_LENGTH;
    }

    return Multihash::create(type, hash);
  }

  const HashType &Multihash::getType() const {
    return data_.type;
  }

  BytesIn Multihash::getHash() const {
    return BytesIn(data_.bytes).subspan(data_.hash_offset);
  }

  std::string Multihash::toHex() const {
    return fmt::format("{:X}", data_.bytes);
  }

  const Bytes &Multihash::toBuffer() const {
    return data_.bytes;
  }

  bool Multihash::operator==(const Multihash &other) const {
    return data_.bytes == other.data_.bytes && data_.type == other.data_.type;
  }

  bool Multihash::operator!=(const Multihash &other) const {
    return !(this->operator==(other));
  }

  bool Multihash::operator<(const class libp2p::multi::Multihash &other) const {
    if (data_.type == other.data_.type) {
      return data_.bytes < other.data_.bytes;
    }
    return data_.type < other.data_.type;
  }

}  // namespace libp2p::multi
