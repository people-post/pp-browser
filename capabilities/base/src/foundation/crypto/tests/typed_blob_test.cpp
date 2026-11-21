// Typed keys and signatures (docs/contracts/SIGNED_EXTENSIONS.md § Typed blobs): pp-browser's reads and writes over
// pp-cpp-crypto's AlgId, their error mapping (an unknown algorithm is "update needed", anything else a refusal), and
// the copy of pp-cpp-crypto's frozen alg_id_vectors.json read through them byte for byte.

#include "foundation/crypto/TypedBlob.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace pbr {
namespace {

using pp::BlobKind;
using pp::kMlDsa65SeedBytes;
using test::VectorFile;

const VectorFile kAlgIdVectors{PP_ALG_ID_VECTORS_PATH, "PP_WRITE_ALG_ID_VECTORS"};

MlDsaKeyPair Keys(const uint8_t fill) {
  return MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, fill)).value();
}

std::optional<BlobKind> BlobKindOfName(const std::string& name) {
  for (const BlobKind kind :
       {BlobKind::kSignaturePublicKey, BlobKind::kSignatureSecretKey, BlobKind::kSignature, BlobKind::kKemPublicKey,
        BlobKind::kKemPrivateKey, BlobKind::kKemCiphertext, BlobKind::kAeadSealed}) {
    if (pp::BlobKindName(kind) == name) {
      return kind;
    }
  }
  return std::nullopt;
}

std::string StatusName(const TypedBlobStatus status) {
  switch (status) {
    case TypedBlobStatus::Ok:
      return "ok";
    case TypedBlobStatus::UnknownAlgorithm:
      return "unknown_algorithm";
    case TypedBlobStatus::Malformed:
      return "malformed";
  }
  return "?";
}

// RC020-TYPED: the four typed blobs signed objects carry are one id byte longer than the raw algorithm bytes.
TEST(TypedBlobTest, TheFourTypedBlobsAreTheirIdThenTheRawBytes) {
  const MlDsaKeyPair keys = Keys(0x01);
  const HybridKemKeyPair kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, 0x02)).value();
  std::string key_init_b64;
  ASSERT_TRUE(HybridKem::Encapsulate(kem.public_key, key_init_b64));
  const ByteVector ciphertext = Base64Decode(key_init_b64).value();
  ASSERT_EQ(ciphertext.size(), kHybridKemCiphertextBytes);
  const ByteVector signature = MlDsa::Sign(keys.secret_key, ByteVector{1, 2, 3}).value();

  const ByteVector typed_key = TypedSigningPublicKey(keys.public_key).value();
  const ByteVector typed_signature = TypedSignature(signature).value();
  const ByteVector typed_kem = TypedKemPublicKey(kem.public_key).value();
  const ByteVector typed_ciphertext = TypedKemCiphertext(ciphertext).value();
  EXPECT_EQ(typed_key.size(), 1953u);
  EXPECT_EQ(typed_signature.size(), 3310u);
  EXPECT_EQ(typed_kem.size(), 1185u);
  EXPECT_EQ(typed_ciphertext.size(), 1089u);
  for (const ByteVector* blob : {&typed_key, &typed_signature, &typed_kem, &typed_ciphertext}) {
    EXPECT_EQ(blob->front(), 1);
  }
  EXPECT_EQ(ByteVector(typed_key.begin() + 1, typed_key.end()), keys.public_key);

  const TypedRead read = ReadTypedBlob(BlobKind::kKemCiphertext, typed_ciphertext, "key_init");
  ASSERT_TRUE(read.ok()) << read.detail;
  EXPECT_EQ(read.alg_id, pp::kKemAlgMlKem768);
  EXPECT_EQ(read.raw, ciphertext);
}

TEST(TypedBlobTest, ThisBuildWritesTheRegistrysFirstIdOfEachKind) {
  EXPECT_EQ(WrittenAlgId(BlobKind::kSignaturePublicKey), pp::kSignatureAlgMlDsa65);
  EXPECT_EQ(WrittenAlgId(BlobKind::kSignature), pp::kSignatureAlgMlDsa65);
  EXPECT_EQ(WrittenAlgId(BlobKind::kKemPublicKey), pp::kKemAlgMlKem768);
  EXPECT_EQ(WrittenAlgId(BlobKind::kKemCiphertext), pp::kKemAlgMlKem768);
  EXPECT_EQ(WrittenAlgId(BlobKind::kAeadSealed), pp::kAeadAlgXChaCha20Poly1305);
}

TEST(TypedBlobTest, AWrongSizedRawValueIsNotWritten) {
  EXPECT_FALSE(TypedSigningPublicKey(ByteVector(kMlDsa65PublicKeyBytes - 1, 0)));
  EXPECT_FALSE(TypedSignature(ByteVector{}));
  EXPECT_FALSE(WriteTypedBlobB64(BlobKind::kKemPublicKey, ByteVector(3, 0)));
}

TEST(TypedBlobTest, TheBase64FormReadsBack) {
  const MlDsaKeyPair keys = Keys(0x03);
  const std::string b64 = WriteTypedBlobB64(BlobKind::kSignaturePublicKey, keys.public_key).value();
  const TypedRead read = ReadTypedBlobB64(BlobKind::kSignaturePublicKey, b64, "public_key");
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(read.raw, keys.public_key);
}

// RC020-ALG / U1: an unknown algorithm id is "update needed" (NewerFormat), whatever follows it.
TEST(TypedBlobTest, AnUnknownAlgorithmIsUpdateNeeded) {
  ByteVector blob(1 + kMlDsa65PublicKeyBytes, 0);
  blob[0] = 2;
  const TypedRead read = ReadTypedBlob(BlobKind::kSignaturePublicKey, blob, "card signing key");
  EXPECT_EQ(read.status, TypedBlobStatus::UnknownAlgorithm);
  EXPECT_TRUE(AppError::IsNewerFormat(read.ToError()));
  EXPECT_NE(read.detail.find("card signing key"), std::string::npos) << read.detail;
  EXPECT_EQ(ReadTypedBlob(BlobKind::kSignature, ByteVector{0xff}, "sig").status, TypedBlobStatus::UnknownAlgorithm);
}

// RC020-ALG: empty, not base64, id 0 or a wrong size is malformed: a refusal, never "update needed".
TEST(TypedBlobTest, AMalformedBlobIsARefusalNotUpdateNeeded) {
  const ByteVector short_key(kMlDsa65PublicKeyBytes, 1); // id 1, one byte short
  ByteVector id_zero(1 + kMlDsa65PublicKeyBytes, 0);
  for (const TypedRead& read : {ReadTypedBlob(BlobKind::kSignaturePublicKey, short_key, "k"),
                                ReadTypedBlob(BlobKind::kSignaturePublicKey, id_zero, "k"),
                                ReadTypedBlob(BlobKind::kSignaturePublicKey, ByteVector{}, "k"),
                                ReadTypedBlobB64(BlobKind::kSignaturePublicKey, "not base64!", "k"),
                                ReadTypedBlobB64(BlobKind::kSignaturePublicKey, "", "k")}) {
    EXPECT_EQ(read.status, TypedBlobStatus::Malformed) << read.detail;
    EXPECT_FALSE(AppError::IsNewerFormat(read.ToError()));
    EXPECT_FALSE(read.ToError().message.empty());
  }
}

TEST(TypedBlobTest, ATypedSignatureVerifiesUnderItsTypedKey) {
  const MlDsaKeyPair keys = Keys(0x04);
  const ByteVector message{9, 8, 7};
  const ByteVector signature = SignTyped(keys.secret_key, message).value();
  const TypedRead key =
      ReadTypedBlob(BlobKind::kSignaturePublicKey, TypedSigningPublicKey(keys.public_key).value(), "key");
  const TypedRead sig = ReadTypedBlob(BlobKind::kSignature, signature, "signature");
  ASSERT_TRUE(key.ok());
  ASSERT_TRUE(sig.ok());
  EXPECT_TRUE(VerifyTyped(key, message, sig));
  EXPECT_FALSE(VerifyTyped(key, ByteVector{9, 8, 6}, sig));

  const TypedRead other =
      ReadTypedBlob(BlobKind::kSignaturePublicKey, TypedSigningPublicKey(Keys(0x05).public_key).value(), "other");
  EXPECT_FALSE(VerifyTyped(other, message, sig));
}

TEST(TypedBlobTest, ASignatureVerifiesOnlyWithAKeyOfItsAlgorithm) {
  const MlDsaKeyPair keys = Keys(0x06);
  const ByteVector message{1};
  TypedRead key = ReadTypedBlob(BlobKind::kSignaturePublicKey, TypedSigningPublicKey(keys.public_key).value(), "k");
  TypedRead sig = ReadTypedBlob(BlobKind::kSignature, SignTyped(keys.secret_key, message).value(), "s");
  sig.alg_id = 2; // as if a later algorithm's signature had been read
  EXPECT_FALSE(VerifyTyped(key, message, sig));
  TypedRead unread;
  EXPECT_FALSE(VerifyTyped(unread, message, sig));
}

// RC020-VEC: the copy of pp-cpp-crypto's vectors, read through pp-browser's helpers: every blob's status, its id and
// raw bytes, and the same bytes written back.
TEST(TypedBlobTest, TheLibrarysAlgIdVectorsReadThroughTheseHelpers) {
  const pp::common::Object vectors = kAlgIdVectors.Read();
  EXPECT_EQ(vectors.getString("format").value_or(""), "pp-cpp-crypto alg-id vectors v1");
  const pp::common::Array* blobs = vectors.getArray("blobs");
  ASSERT_NE(blobs, nullptr);
  ASSERT_FALSE(blobs->elements.empty());
  size_t ok = 0;
  for (const pp::common::Value& item : blobs->elements) {
    const pp::common::Object* row = pp::common::asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string note = row->getString("note").value_or("");
    const std::optional<BlobKind> kind = BlobKindOfName(row->getString("blob_kind").value_or(""));
    ASSERT_TRUE(kind.has_value()) << note;
    const ByteVector blob = HexToBytes(row->getString("blob_hex").value_or("")).value();
    const TypedRead read = ReadTypedBlob(*kind, blob, note);
    const std::string expected = row->getString("result").value_or("");
    EXPECT_EQ(StatusName(read.status), expected) << note;
    EXPECT_EQ(AppError::IsNewerFormat(read.ToError()), expected == "unknown_algorithm") << note;
    if (read.ok()) {
      ++ok;
      EXPECT_EQ(int64_t{read.alg_id}, row->getIf<int64_t>("id").value_or(-1)) << note;
      EXPECT_EQ(static_cast<int64_t>(read.raw.size()), row->getIf<int64_t>("body_bytes").value_or(-1)) << note;
      // This build writes every registry id it reads (one per kind at release 1): the bytes come back the same.
      EXPECT_EQ(WriteTypedBlob(*kind, read.raw).value(), blob) << note;
    }
  }
  EXPECT_EQ(ok, 8u); // one per blob kind, and a second AEAD blob
}

TEST(TypedBlobTest, TheLibrarysUnknownIdsAreUpdateNeededForEveryBlobOfTheirKind) {
  const pp::common::Object vectors = kAlgIdVectors.Read();
  const pp::common::Array* unknown = vectors.getArray("unknown_ids");
  ASSERT_NE(unknown, nullptr);
  for (const pp::common::Value& item : unknown->elements) {
    const pp::common::Object* row = pp::common::asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string kind = row->getString("kind").value_or("");
    const auto id = static_cast<uint8_t>(row->getIf<int64_t>("id").value_or(0));
    if (kind != "signature" && kind != "kem") {
      continue; // aead blobs are not read here; hash and kdf ids are never written into blobs
    }
    const BlobKind blob_kind = kind == "signature" ? BlobKind::kSignature : BlobKind::kKemCiphertext;
    EXPECT_EQ(ReadTypedBlob(blob_kind, ByteVector{id, 0, 0}, kind).status, TypedBlobStatus::UnknownAlgorithm)
        << kind << " " << int{id};
  }
}

} // namespace
} // namespace pbr
