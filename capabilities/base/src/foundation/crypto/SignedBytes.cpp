#include "foundation/crypto/SignedBytes.h"

#include "foundation/crypto/CryptoUtil.h"

#include "common/Serialize.hpp"

#include <cassert>

namespace pbr {

SignedBytesWriter::SignedBytesWriter(const std::string_view domain, const uint8_t version) {
  assert(IsSignedDomain(domain));
  out_.write(domain.data(), static_cast<std::streamsize>(domain.size()));
  out_.put('\0');
  U8(version);
}

SignedBytesWriter& SignedBytesWriter::U8(const uint8_t value) {
  pp::OutputArchive ar(out_);
  ar & value;
  return *this;
}

SignedBytesWriter& SignedBytesWriter::U32(const uint32_t value) {
  pp::OutputArchive ar(out_);
  ar & value;
  return *this;
}

SignedBytesWriter& SignedBytesWriter::U64(const uint64_t value) {
  pp::OutputArchive ar(out_);
  ar & value;
  return *this;
}

SignedBytesWriter& SignedBytesWriter::I64(const int64_t value) {
  pp::OutputArchive ar(out_);
  ar & value;
  return *this;
}

SignedBytesWriter& SignedBytesWriter::Utf8(const std::string_view value) {
  pp::WireLenUtf8 wire{std::string(value)};
  pp::OutputArchive ar(out_);
  ar & wire;
  return *this;
}

SignedBytesWriter& SignedBytesWriter::Bytes(const std::span<const uint8_t> value) {
  pp::WireLenBytes wire{ByteVector(value.begin(), value.end())};
  pp::OutputArchive ar(out_);
  ar & wire;
  return *this;
}

SignedBytesWriter& SignedBytesWriter::Raw(const std::span<const uint8_t> value) {
  out_.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value.size()));
  return *this;
}

ByteVector SignedBytesWriter::Finish(const pp::ExtArea& ext) {
  const std::string packed = out_.str();
  ByteVector bytes(packed.begin(), packed.end());
  ext.AppendTo(bytes);
  return bytes;
}

bool SignedBlobReader::Take(const size_t n, std::span<const uint8_t>* out) {
  if (!ok_ || n > bytes_.size() - offset_) {
    ok_ = false;
    return false;
  }
  *out = bytes_.subspan(offset_, n);
  offset_ += n;
  return true;
}

uint64_t SignedBlobReader::BigEndian(const size_t n) {
  std::span<const uint8_t> field;
  if (!Take(n, &field)) {
    return 0;
  }
  uint64_t value = 0;
  for (const uint8_t byte : field) {
    value = (value << 8) | byte;
  }
  return value;
}

uint8_t SignedBlobReader::U8() {
  return static_cast<uint8_t>(BigEndian(1));
}

uint32_t SignedBlobReader::U32() {
  return static_cast<uint32_t>(BigEndian(4));
}

uint64_t SignedBlobReader::U64() {
  return BigEndian(8);
}

int64_t SignedBlobReader::I64() {
  return static_cast<int64_t>(BigEndian(8));
}

ByteVector SignedBlobReader::Raw(const size_t n) {
  std::span<const uint8_t> field;
  if (!Take(n, &field)) {
    return {};
  }
  return ByteVector(field.begin(), field.end());
}

ByteVector SignedBlobReader::Bytes() {
  const uint64_t length = U64();
  if (!ok_ || length > bytes_.size() - offset_) {
    ok_ = false;
    return {};
  }
  return Raw(static_cast<size_t>(length));
}

std::string SignedBlobReader::Utf8() {
  const ByteVector value = Bytes();
  return std::string(value.begin(), value.end());
}

pp::ExtArea SignedBlobReader::Ext() {
  if (!ok_) {
    return {};
  }
  auto ext = pp::ExtArea::DecodeAt(bytes_, &offset_);
  if (!ext) {
    ok_ = false;
    return {};
  }
  return std::move(ext.value());
}

bool IsSignedDomain(const std::string_view domain) {
  if (domain.empty()) {
    return false;
  }
  for (const char c : domain) {
    if (c < 0x21 || c > 0x7e) {
      return false;
    }
  }
  // Not `…-v<digits>`: the version is the byte after the domain (SIGNED_FORMATS Q3).
  size_t digits = 0;
  while (digits < domain.size() && domain[domain.size() - 1 - digits] >= '0' &&
         domain[domain.size() - 1 - digits] <= '9') {
    ++digits;
  }
  const size_t rest = domain.size() - digits;
  return digits == 0 || rest < 2 || domain.substr(rest - 2, 2) != "-v";
}

std::optional<uint8_t> SignedBytesVersion(const std::span<const uint8_t> signed_bytes) {
  for (size_t i = 0; i + 1 < signed_bytes.size(); ++i) {
    if (signed_bytes[i] == 0) {
      if (i == 0) {
        return std::nullopt; // a domain is never empty
      }
      return signed_bytes[i + 1];
    }
  }
  return std::nullopt;
}

Roe<pp::ExtArea> ReadExtB64(const pp::common::Object& object) {
  if (!object.contains(kExtB64Key)) {
    return pp::ExtArea{};
  }
  const std::optional<std::string> text = object.getString(kExtB64Key);
  if (!text) {
    return Error("ext_b64: not a string");
  }
  if (text->empty()) {
    return pp::ExtArea{};
  }
  auto entries = Base64Decode(*text);
  if (!entries) {
    return Error("ext_b64: not base64");
  }
  auto ext = pp::ExtArea::DecodeEntries(entries.value());
  if (!ext) {
    return Error("ext_b64: " + std::string(pp::ExtArea::ErrName(ext.error().GetCode())));
  }
  return std::move(ext.value());
}

void WriteExtB64(pp::common::Object& object, const pp::ExtArea& ext) {
  if (ext.empty()) {
    object.erase(kExtB64Key);
    return;
  }
  object.set(kExtB64Key, Base64Encode(ext.EncodeEntries()));
}

} // namespace pbr
