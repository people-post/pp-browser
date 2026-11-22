#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "common/ExtArea.h"
#include "common/Value.h"

#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Signed bytes (docs/contracts/SIGNED_EXTENSIONS.md § Signed bytes): every signed object's bytes start the same way and
 * end with its extension area, in the pp binary wire profile (pp-cpp-common `Serialize.hpp`, big endian):
 *
 *   domain (ASCII) ‖ 0x00 ‖ u8 version ‖ …the object's fields… ‖ ext area (u64 BE length ‖ entries)
 *
 * The domain carries no `-vN` suffix (the version is the byte after it). The version is the object's outside version
 * (its JSON `*_version` field or its blob's first byte): a receiver rebuilds the bytes with the version it read, so a
 * changed or stripped outside version fails the signature.
 *
 *   ByteVector bytes = SignedBytesWriter("pp-browser:relay-grant", 2)
 *                          .Utf8(recipient_account_id).Utf8(sender_account_id).I64(issued_at).I64(expires_at)
 *                          .Finish(ext);
 */
class SignedBytesWriter {
public:
  /** `domain ‖ 0x00 ‖ u8 version`. `domain` must pass `IsSignedDomain` (checked in debug builds). */
  SignedBytesWriter(std::string_view domain, uint8_t version);

  SignedBytesWriter& U8(uint8_t value);
  /** u32 BE. */
  SignedBytesWriter& U32(uint32_t value);
  /** u64 BE. */
  SignedBytesWriter& U64(uint64_t value);
  /** i64 BE, two's complement. */
  SignedBytesWriter& I64(int64_t value);
  /** LenUtf8: u64 BE byte length, then the UTF-8 bytes. */
  SignedBytesWriter& Utf8(std::string_view value);
  /** LenBytes: u64 BE byte length, then the bytes. */
  SignedBytesWriter& Bytes(std::span<const uint8_t> value);
  /** A typed blob (`TypedBlob.h`): `LenBytes(typed blob)`, as its size varies by algorithm. */
  SignedBytesWriter& Typed(std::span<const uint8_t> typed_blob) { return Bytes(typed_blob); }
  /**
   * Bytes as they are, no length: a field whose size the version fixes (a 32-byte hash), or an object's own encoding
   * where it keeps one (the group objects' u32-length strings).
   */
  SignedBytesWriter& Raw(std::span<const uint8_t> value);

  /** Append the extension area, always the last field, and give the bytes (the writer is done: do not use it again). */
  ByteVector Finish(const pp::ExtArea& ext);

private:
  std::ostringstream out_;
};

/**
 * Reads a binary signed blob's fields in the same encodings (`u8 version ‖ ext area ‖ LenBytes signature` for the KEM
 * binding, the relay certificate's fields). The first failure sticks: every later read gives a zero value, and
 * `Done()` is false. Lengths are bounded by the input, never trusted to allocate.
 *
 *   SignedBlobReader in(blob);
 *   const uint8_t version = in.U8();      // the outside version: compare it before reading on
 *   pp::ExtArea ext = in.Ext();
 *   ByteVector signature = in.Bytes();
 *   if (!in.Done()) { malformed }         // a read failed, or bytes are left over
 */
class SignedBlobReader {
public:
  /** Reads `bytes` in place: they must outlive the reader. */
  explicit SignedBlobReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
  /** Not over a temporary (the span would dangle). */
  explicit SignedBlobReader(ByteVector&&) = delete;

  uint8_t U8();
  uint32_t U32();
  uint64_t U64();
  int64_t I64();
  /** LenUtf8 (the bytes are not checked to be UTF-8). */
  std::string Utf8();
  /** LenBytes; a typed blob too (read it with `ReadTypedBlob`). */
  ByteVector Bytes();
  /** `n` bytes as they are (a field whose size the version fixes). */
  ByteVector Raw(size_t n);
  /** The extension area form (`ExtArea::DecodeAt`); a refused area fails the reader. */
  pp::ExtArea Ext();

  /** Nothing failed so far. */
  bool ok() const { return ok_; }
  /** Nothing failed and every byte was read. */
  bool Done() const { return ok_ && offset_ == bytes_.size(); }
  /** Where the next read starts. */
  size_t offset() const { return offset_; }

private:
  bool Take(size_t n, std::span<const uint8_t>* out);
  uint64_t BigEndian(size_t n);

  std::span<const uint8_t> bytes_;
  size_t offset_ = 0;
  bool ok_ = true;
};

/** A signing domain of this cutover: non-empty printable ASCII, no NUL, not ending in `-v<digits>`. */
bool IsSignedDomain(std::string_view domain);

/** The version byte after the domain's NUL in `signed_bytes`; nullopt when there is none (tests read it). */
std::optional<uint8_t> SignedBytesVersion(std::span<const uint8_t> signed_bytes);

/** The JSON key every signed JSON object carries its extension entries in. */
inline constexpr char kExtB64Key[] = "ext_b64";

/**
 * A JSON object's extension area: `ext_b64` is the entries (no u64 length) in standard base64. Absent, or `""`, is
 * the empty area. Not a string, not base64, or entries that do not decode (SIGNED_EXTENSIONS § The extension area)
 * is malformed: a plain refusal (step 2 of the receiver's order).
 */
Roe<pp::ExtArea> ReadExtB64(const pp::common::Object& object);
/** Set `ext_b64` from `ext`; an empty area leaves the key out (and removes one that is there). */
void WriteExtB64(pp::common::Object& object, const pp::ExtArea& ext);

} // namespace pbr
