/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <libp2p/common/types.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace libp2p::wire {

enum class WireType : uint8_t {
  kVarint = 0,
  kFixed64 = 1,
  kLengthDelimited = 2,
  kFixed32 = 5,
};

struct FieldTag {
  uint32_t number = 0;
  WireType type = WireType::kVarint;
};

class Writer {
 public:
  void writeVarint(uint64_t value);
  void writeTag(uint32_t field_number, WireType type);
  void writeBytesField(uint32_t field_number, BytesIn data);
  void writeStringField(uint32_t field_number, std::string_view value);
  void writeBoolField(uint32_t field_number, bool value);
  void writeVarintField(uint32_t field_number, uint64_t value);
  void writeSubmessageField(uint32_t field_number, BytesIn encoded_submessage);
  [[nodiscard]] Bytes encoded() const;
  [[nodiscard]] Bytes take();

 private:
  std::vector<uint8_t> out_;
};

class Reader {
 public:
  explicit Reader(BytesIn data);

  [[nodiscard]] bool eof() const;
  outcome::result<FieldTag> readTag();
  outcome::result<uint64_t> readVarint();
  outcome::result<Bytes> readLengthDelimited();
  outcome::result<void> skipField(WireType type);

 private:
  BytesIn data_;
  size_t pos_ = 0;
};

enum class DecodeError {
  TRUNCATED = 1,
  INVALID_WIRE_TYPE,
  MALFORMED,
  MISSING_REQUIRED_FIELD,
  UNKNOWN_ENUM,
};

}  // namespace libp2p::wire

OUTCOME_HPP_DECLARE_ERROR(libp2p::wire, DecodeError);
