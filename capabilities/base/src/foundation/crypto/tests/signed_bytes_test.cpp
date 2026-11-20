// Signed bytes and the extension area (docs/contracts/SIGNED_EXTENSIONS.md § Signed bytes, § The extension area): the
// writer's header and field encodings (the pp binary wire profile), the blob reader, `ext_b64` in JSON, and the copy of
// pp-cpp-common's frozen ext_area_vectors.json read through them byte for byte.

#include "foundation/crypto/SignedBytes.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"

#include "common/Serialize.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace pbr {
namespace {

using pp::ExtArea;
using pp::common::Object;
using test::VectorFile;

const VectorFile kExtAreaVectors{PP_EXT_AREA_VECTORS_PATH, "PP_WRITE_EXT_AREA_VECTORS"};

ByteVector Hex(const std::string& hex) {
  return HexToBytes(hex).value();
}

ExtArea Area(std::vector<ExtArea::Entry> entries) {
  return ExtArea::FromEntries(std::move(entries)).value();
}

// RC020-BYTES: domain ‖ 00 ‖ u8 version ‖ fields ‖ ext area, every integer big endian, every length a u64.
TEST(SignedBytesTest, TheHeaderFieldsAndAreaAreThePpWireProfile) {
  const ByteVector bytes = SignedBytesWriter("pp:x", 2)
                               .U8(0xab)
                               .U32(0x01020304)
                               .U64(5)
                               .I64(-1)
                               .Utf8("hi")
                               .Bytes(ByteVector{0xcc})
                               .Typed(ByteVector{1, 0xdd})
                               .Raw(ByteVector{0xee, 0xff})
                               .Finish(ExtArea{});
  EXPECT_EQ(BytesToHex(bytes), std::string("70703a78") + "00" + "02" // domain, NUL, version
                                   + "ab" + "01020304" + "0000000000000005" + "ffffffffffffffff" +
                                   "00000000000000026869"   // LenUtf8 "hi"
                                   + "0000000000000001cc"   // LenBytes
                                   + "000000000000000201dd" // a typed blob is LenBytes
                                   + "eeff"                 // raw
                                   + "0000000000000000");   // the empty area, last
}

TEST(SignedBytesTest, TheWritersLengthsAreSerializesOwn) {
  std::ostringstream oss;
  pp::OutputArchive ar(oss);
  pp::WireLenUtf8 text{"héllo"};
  pp::WireLenBytes blob{ByteVector{1, 2, 3}};
  ar & text;
  ar & blob;
  const std::string packed = oss.str();
  const ByteVector expected(packed.begin(), packed.end());
  const ByteVector bytes = SignedBytesWriter("d", 1).Utf8("héllo").Bytes(ByteVector{1, 2, 3}).Finish(ExtArea{});
  const ByteVector fields(bytes.begin() + 3, bytes.end() - 8); // past "d" 00 01, before the area
  EXPECT_EQ(fields, expected);
}

TEST(SignedBytesTest, TheAreaIsAlwaysLast) {
  const ExtArea ext = Area({{.type = 0x0001, .value = {1, 2, 3}}});
  const ByteVector bytes = SignedBytesWriter("pp:x", 1).U8(7).Finish(ext);
  EXPECT_EQ(BytesToHex(bytes), "70703a78000107" + std::string("0000000000000009000100000003010203"));
  // Another area is other bytes: the signature covers it.
  EXPECT_NE(bytes, SignedBytesWriter("pp:x", 1).U8(7).Finish(ExtArea{}));
}

TEST(SignedBytesTest, TheVersionIsTheByteAfterTheDomain) {
  EXPECT_EQ(SignedBytesVersion(SignedBytesWriter("pp-browser:relay-grant", 2).Finish(ExtArea{})), 2);
  EXPECT_EQ(SignedBytesVersion(Hex("00ff")), std::nullopt); // an empty domain
  EXPECT_EQ(SignedBytesVersion(Hex("616200")), std::nullopt);
  EXPECT_EQ(SignedBytesVersion(ByteVector{}), std::nullopt);
}

// RC020 Q3: domains lose their -vN suffix.
TEST(SignedBytesTest, DomainsCarryNoVersionSuffix) {
  EXPECT_TRUE(IsSignedDomain("pp-browser:account-card"));
  EXPECT_TRUE(IsSignedDomain("pp-browser:relay-locker-put"));
  EXPECT_TRUE(IsSignedDomain("pp-browser:v4"));
  EXPECT_TRUE(IsSignedDomain("pp-browser:x-v"));
  EXPECT_TRUE(IsSignedDomain("pp-browser:ipv6"));
  EXPECT_FALSE(IsSignedDomain("pp-browser:account-card-v4"));
  EXPECT_FALSE(IsSignedDomain("pp-browser:relay-envelope-sign-v5"));
  EXPECT_FALSE(IsSignedDomain("pp-browser:relay-revoke-v12"));
  EXPECT_FALSE(IsSignedDomain(""));
  EXPECT_FALSE(IsSignedDomain("pp browser"));
  EXPECT_FALSE(IsSignedDomain(std::string_view("pp\0x", 4)));
}

TEST(SignedBytesTest, TheBlobReaderReadsWhatTheWriterWrites) {
  const ExtArea ext = Area({{.type = 0x0002, .value = {9}}});
  const ByteVector bytes = SignedBytesWriter("pp:x", 3)
                               .U8(1)
                               .U32(77)
                               .U64(1ULL << 40)
                               .I64(-42)
                               .Utf8("relay")
                               .Bytes(ByteVector{4, 5})
                               .Raw(ByteVector{6, 7, 8})
                               .Finish(ext);
  SignedBlobReader in(std::span<const uint8_t>(bytes).subspan(5)); // past "pp:x" 00
  EXPECT_EQ(in.U8(), 3);
  EXPECT_EQ(in.U8(), 1);
  EXPECT_EQ(in.U32(), 77u);
  EXPECT_EQ(in.U64(), 1ULL << 40);
  EXPECT_EQ(in.I64(), -42);
  EXPECT_EQ(in.Utf8(), "relay");
  EXPECT_EQ(in.Bytes(), (ByteVector{4, 5}));
  EXPECT_EQ(in.Raw(3), (ByteVector{6, 7, 8}));
  EXPECT_EQ(in.Ext(), ext);
  EXPECT_TRUE(in.Done());
}

TEST(SignedBytesTest, TheBlobReadersFirstFailureSticks) {
  const ByteVector blob =
      Hex("02"
          "00000000000000ff"
          "01"); // a length past the end
  SignedBlobReader in(blob);
  EXPECT_EQ(in.U8(), 2);
  EXPECT_TRUE(in.Bytes().empty());
  EXPECT_FALSE(in.ok());
  EXPECT_EQ(in.U8(), 0); // nothing more is read
  EXPECT_FALSE(in.Done());

  const ByteVector two_bytes = Hex("0201");
  SignedBlobReader left_over(two_bytes);
  EXPECT_EQ(left_over.U8(), 2);
  EXPECT_TRUE(left_over.ok());
  EXPECT_FALSE(left_over.Done());

  const ByteVector area = Hex("00000000000000038000");
  SignedBlobReader bad_area(area);
  bad_area.Ext();
  EXPECT_FALSE(bad_area.ok());
}

// RC020-EXT: JSON carries the entries as `ext_b64`; absent and "" are the empty area; writers leave it out when empty.
TEST(SignedBytesTest, ExtB64AbsentOrEmptyIsTheEmptyArea) {
  Object object;
  EXPECT_TRUE(ReadExtB64(object).value().empty());
  object.set(kExtB64Key, std::string());
  EXPECT_TRUE(ReadExtB64(object).value().empty());

  WriteExtB64(object, ExtArea{});
  EXPECT_FALSE(object.contains(kExtB64Key));

  const ExtArea ext = Area({{.type = 0x0001, .value = {1, 2, 3}}});
  WriteExtB64(object, ext);
  EXPECT_EQ(object.getString(kExtB64Key).value_or(""), "AAEAAAADAQID");
  EXPECT_EQ(ReadExtB64(object).value(), ext);
  WriteExtB64(object, ExtArea{});
  EXPECT_FALSE(object.contains(kExtB64Key));
}

TEST(SignedBytesTest, ExtB64ThatDoesNotDecodeIsARefusal) {
  Object not_string;
  not_string.set(kExtB64Key, int64_t{1});
  EXPECT_FALSE(ReadExtB64(not_string));
  Object not_base64;
  not_base64.set(kExtB64Key, std::string("***"));
  EXPECT_FALSE(ReadExtB64(not_base64));
  Object unsorted;
  unsorted.set(kExtB64Key, Base64Encode(Hex("000200000000"
                                            "000100000000")));
  auto refused = ReadExtB64(unsorted);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().message.find("unsorted"), std::string::npos) << refused.error().message;
}

// RC020-VEC: the copy of pp-cpp-common's vectors, through pp-browser's helpers: every case's `ext_b64` reads to its
// result, an accepted area writes back to the same `ext_b64` and, last in signed bytes, to its `area_hex`.
TEST(SignedBytesTest, TheLibrarysExtAreaVectorsReadThroughTheseHelpers) {
  const Object vectors = kExtAreaVectors.Read();
  EXPECT_EQ(vectors.getString("format").value_or(""), "pp-cpp-common ext area vectors v1");
  const pp::common::Array* cases = vectors.getArray("cases");
  ASSERT_NE(cases, nullptr);
  ASSERT_FALSE(cases->elements.empty());
  for (const pp::common::Value& item : cases->elements) {
    const Object* row = pp::common::asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("name").value_or("");
    const std::string result = row->getString("result").value_or("");
    Object object;
    object.set(kExtB64Key, row->getString("ext_b64").value_or(""));
    auto read = ReadExtB64(object);
    ASSERT_EQ(static_cast<bool>(read), result == "ok") << name;
    if (!read) {
      EXPECT_NE(read.error().message.find(result), std::string::npos) << name << ": " << read.error().message;
      continue;
    }
    Object written;
    WriteExtB64(written, read.value());
    EXPECT_EQ(written.getString(kExtB64Key).value_or(""), row->getString("ext_b64").value_or("")) << name;
    const ByteVector bytes = SignedBytesWriter("d", 1).Finish(read.value());
    EXPECT_EQ(BytesToHex(ByteVector(bytes.begin() + 3, bytes.end())), row->getString("area_hex").value_or("")) << name;
  }
}

// RC020-VEC: the area form inside a binary blob, as the blob reader reads it.
TEST(SignedBytesTest, TheLibrarysAreaCasesReadThroughTheBlobReader) {
  const Object vectors = kExtAreaVectors.Read();
  const pp::common::Array* cases = vectors.getArray("area_cases");
  ASSERT_NE(cases, nullptr);
  for (const pp::common::Value& item : cases->elements) {
    const Object* row = pp::common::asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("name").value_or("");
    const ByteVector area = Hex(row->getString("area_hex").value_or(""));
    SignedBlobReader in(area);
    in.Ext();
    EXPECT_FALSE(in.Done()) << name; // every area case is a refusal (bytes after the area included)
  }
  const pp::common::Array* ok_cases = vectors.getArray("cases");
  ASSERT_NE(ok_cases, nullptr);
  for (const pp::common::Value& item : ok_cases->elements) {
    const Object* row = pp::common::asObject(item);
    ASSERT_NE(row, nullptr);
    const ByteVector area = Hex(row->getString("area_hex").value_or(""));
    SignedBlobReader in(area);
    in.Ext();
    EXPECT_EQ(in.Done(), row->getString("result").value_or("") == "ok") << row->getString("name").value_or("");
  }
}

} // namespace
} // namespace pbr
