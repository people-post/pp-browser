/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/wire_codec.hpp>

namespace libp2p::wire {

OUTCOME_CPP_DEFINE_CATEGORY(libp2p::wire, DecodeError, e) {
  using E = DecodeError;
  switch (e) {
    case E::TRUNCATED:
      return "truncated protobuf wire data";
    case E::INVALID_WIRE_TYPE:
      return "invalid protobuf wire type";
    case E::MALFORMED:
      return "malformed protobuf wire data";
    case E::MISSING_REQUIRED_FIELD:
      return "missing required protobuf field";
    case E::UNKNOWN_ENUM:
      return "unknown protobuf enum value";
  }
  return "unknown wire decode error";
}

void Writer::writeVarint(uint64_t value) {
  while (value >= 0x80) {
    out_.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out_.push_back(static_cast<uint8_t>(value));
}

void Writer::writeTag(uint32_t field_number, WireType type) {
  writeVarint((static_cast<uint64_t>(field_number) << 3)
              | static_cast<uint64_t>(type));
}

void Writer::writeBytesField(uint32_t field_number, BytesIn data) {
  if (data.empty()) {
    return;
  }
  writeTag(field_number, WireType::kLengthDelimited);
  writeVarint(data.size());
  out_.insert(out_.end(), data.begin(), data.end());
}

void Writer::writeStringField(uint32_t field_number, std::string_view value) {
  if (value.empty()) {
    return;
  }
  writeBytesField(field_number,
                  BytesIn(reinterpret_cast<const uint8_t *>(value.data()),
                          value.size()));
}

void Writer::writeBoolField(uint32_t field_number, bool value) {
  writeTag(field_number, WireType::kVarint);
  writeVarint(value ? 1 : 0);
}

void Writer::writeVarintField(uint32_t field_number, uint64_t value) {
  if (value == 0) {
    return;
  }
  writeTag(field_number, WireType::kVarint);
  writeVarint(value);
}

void Writer::writeSubmessageField(uint32_t field_number,
                                  BytesIn encoded_submessage) {
  if (encoded_submessage.empty()) {
    return;
  }
  writeTag(field_number, WireType::kLengthDelimited);
  writeVarint(encoded_submessage.size());
  out_.insert(out_.end(), encoded_submessage.begin(), encoded_submessage.end());
}

Bytes Writer::encoded() const {
  return out_;
}

Bytes Writer::take() {
  return std::move(out_);
}

Reader::Reader(BytesIn data) : data_(data) {}

bool Reader::eof() const {
  return pos_ >= data_.size();
}

outcome::result<FieldTag> Reader::readTag() {
  if (eof()) {
    return DecodeError::TRUNCATED;
  }
  OUTCOME_TRY(key, readVarint());
  const auto wire = static_cast<uint8_t>(key & 0x07);
  if (wire > 5 || wire == 3 || wire == 4) {
    return DecodeError::INVALID_WIRE_TYPE;
  }
  FieldTag tag;
  tag.number = static_cast<uint32_t>(key >> 3);
  tag.type = static_cast<WireType>(wire);
  return tag;
}

outcome::result<uint64_t> Reader::readVarint() {
  uint64_t value = 0;
  unsigned shift = 0;
  while (shift <= 63) {
    if (pos_ >= data_.size()) {
      return DecodeError::TRUNCATED;
    }
    const uint8_t byte = data_[pos_++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      return value;
    }
    shift += 7;
  }
  return DecodeError::MALFORMED;
}

outcome::result<Bytes> Reader::readLengthDelimited() {
  OUTCOME_TRY(len_u, readVarint());
  const size_t len = static_cast<size_t>(len_u);
  if (pos_ + len > data_.size()) {
    return DecodeError::TRUNCATED;
  }
  Bytes out(data_.begin() + static_cast<std::ptrdiff_t>(pos_),
            data_.begin() + static_cast<std::ptrdiff_t>(pos_ + len));
  pos_ += len;
  return out;
}

outcome::result<void> Reader::skipField(WireType type) {
  switch (type) {
    case WireType::kVarint: {
      OUTCOME_TRY(readVarint());
      return outcome::success();
    }
    case WireType::kFixed64: {
      if (pos_ + 8 > data_.size()) {
        return DecodeError::TRUNCATED;
      }
      pos_ += 8;
      return outcome::success();
    }
    case WireType::kLengthDelimited: {
      OUTCOME_TRY(readLengthDelimited());
      return outcome::success();
    }
    case WireType::kFixed32: {
      if (pos_ + 4 > data_.size()) {
        return DecodeError::TRUNCATED;
      }
      pos_ += 4;
      return outcome::success();
    }
  }
  return DecodeError::INVALID_WIRE_TYPE;
}

}  // namespace libp2p::wire
