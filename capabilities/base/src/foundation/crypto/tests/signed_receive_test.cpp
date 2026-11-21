// The receiver's order (docs/contracts/SIGNED_EXTENSIONS.md § The receiver's order): version → parse → algorithm ids →
// verify → extensions, each step's verdict and its error (update needed is AppError::NewerFormat, every other refusal a
// plain Error), and one whole signed JSON object built and received with the W0 helpers, the shape W1–W6 follow.

#include "foundation/crypto/SignedReceive.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/TypedBlob.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

using pp::ExtArea;
using pp::kMlDsa65SeedBytes;
using pp::common::Object;

constexpr SignedVersions kOnlyTwo{.floor = 2, .current = 2};

Roe<void> Pass() {
  return {};
}

/** Steps that all pass, recording which ran. */
struct Recorder {
  std::vector<std::string> ran;
  ExtArea ext;

  SignedReceiveSteps Steps(std::optional<int64_t> version) {
    return SignedReceiveSteps{.what = "test object",
                              .outside_version = version,
                              .versions = kOnlyTwo,
                              .parse = [this] { return Ran("parse"); },
                              .algorithms = [this] { return Ran("algorithms"); },
                              .verify = [this] { return Ran("verify"); },
                              .ext = &ext};
  }

  Roe<void> Ran(const std::string& step) {
    ran.push_back(step);
    return {};
  }
};

ExtArea Critical() {
  return ExtArea::FromEntries({{.type = 0x8001, .value = {1}}}).value();
}

TEST(SignedReceiveTest, AllStepsPassingIsAccepted) {
  Recorder recorder;
  const SignedReceipt receipt = ReceiveSigned(recorder.Steps(2));
  EXPECT_TRUE(receipt.accepted());
  EXPECT_EQ(receipt.version, 2);
  EXPECT_TRUE(receipt.ToRoe());
  EXPECT_EQ(recorder.ran, (std::vector<std::string>{"parse", "algorithms", "verify"}));
}

// RC020-ORDER step 1: a newer version is update needed before anything is parsed or verified.
TEST(SignedReceiveTest, ANewerVersionIsUpdateNeededBeforeVerifying) {
  Recorder recorder;
  const SignedReceipt receipt = ReceiveSigned(recorder.Steps(3));
  EXPECT_EQ(receipt.verdict, SignedVerdict::NeedsUpdate);
  EXPECT_TRUE(receipt.needs_update());
  EXPECT_TRUE(AppError::IsNewerFormat(receipt.ToError()));
  EXPECT_TRUE(recorder.ran.empty());
  EXPECT_NE(receipt.detail.find("test object v3"), std::string::npos) << receipt.detail;
}

// RC020-ORDER step 1: missing or below the floor is an older app's: refused, never "update needed".
TEST(SignedReceiveTest, AMissingOrOlderVersionIsRefusedAsOlder) {
  for (const std::optional<int64_t> version : {std::optional<int64_t>{}, std::optional<int64_t>{1}}) {
    Recorder recorder;
    const SignedReceipt receipt = ReceiveSigned(recorder.Steps(version));
    EXPECT_EQ(receipt.verdict, SignedVerdict::Older);
    EXPECT_FALSE(AppError::IsNewerFormat(receipt.ToError()));
    EXPECT_FALSE(receipt.ToRoe());
    EXPECT_TRUE(recorder.ran.empty());
  }
}

TEST(SignedReceiveTest, AParseFailureIsMalformedAndNothingIsVerified) {
  Recorder recorder;
  SignedReceiveSteps steps = recorder.Steps(2);
  steps.parse = [] { return Roe<void>(Error("bad field")); };
  const SignedReceipt receipt = ReceiveSigned(steps);
  EXPECT_EQ(receipt.verdict, SignedVerdict::Malformed);
  EXPECT_FALSE(AppError::IsNewerFormat(receipt.ToError()));
  EXPECT_NE(receipt.detail.find("bad field"), std::string::npos);
  EXPECT_TRUE(recorder.ran.empty());
}

// RC020-ORDER step 3: an unknown algorithm id (a typed blob's NewerFormat) is update needed, before verifying.
TEST(SignedReceiveTest, AnUnknownAlgorithmIsUpdateNeededBeforeVerifying) {
  Recorder recorder;
  SignedReceiveSteps steps = recorder.Steps(2);
  steps.algorithms = [] {
    const TypedRead key = ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, ByteVector{7, 0}, "signing key");
    return key.ok() ? Roe<void>{} : Roe<void>(key.ToError());
  };
  const SignedReceipt receipt = ReceiveSigned(steps);
  EXPECT_EQ(receipt.verdict, SignedVerdict::NeedsUpdate);
  EXPECT_TRUE(AppError::IsNewerFormat(receipt.ToError()));
  EXPECT_EQ(recorder.ran, (std::vector<std::string>{"parse"}));
}

TEST(SignedReceiveTest, AMalformedTypedBlobIsMalformed) {
  Recorder recorder;
  SignedReceiveSteps steps = recorder.Steps(2);
  steps.algorithms = [] {
    const TypedRead key = ReadTypedBlob(pp::BlobKind::kSignaturePublicKey, ByteVector{1, 0}, "signing key");
    return key.ok() ? Roe<void>{} : Roe<void>(key.ToError());
  };
  EXPECT_EQ(ReceiveSigned(steps).verdict, SignedVerdict::Malformed);
}

TEST(SignedReceiveTest, ANewerFormatFromParseIsUpdateNeeded) {
  Recorder recorder;
  SignedReceiveSteps steps = recorder.Steps(2);
  steps.parse = [] { return Roe<void>(AppError::NewerFormat("nested copy v9")); };
  EXPECT_EQ(ReceiveSigned(steps).verdict, SignedVerdict::NeedsUpdate);
  EXPECT_TRUE(recorder.ran.empty());
}

TEST(SignedReceiveTest, ASignatureThatDoesNotVerifyIsForged) {
  Recorder recorder;
  SignedReceiveSteps steps = recorder.Steps(2);
  steps.verify = [] { return Roe<void>(AppError::NewerFormat("a verifier never says this")); };
  const SignedReceipt receipt = ReceiveSigned(steps);
  EXPECT_EQ(receipt.verdict, SignedVerdict::Forged);
  EXPECT_FALSE(AppError::IsNewerFormat(receipt.ToError()));
  steps.verify = nullptr;
  EXPECT_EQ(ReceiveSigned(steps).verdict, SignedVerdict::Forged);
}

// RC020-ORDER step 5: an unknown critical entry is update needed only once the signature verifies, so an unsigned
// object cannot raise it.
TEST(SignedReceiveTest, AnUnknownCriticalEntryIsReadOnlyAfterTheSignature) {
  Recorder recorder;
  recorder.ext = Critical();
  SignedReceiveSteps steps = recorder.Steps(2);
  EXPECT_EQ(ReceiveSigned(steps).verdict, SignedVerdict::NeedsUpdate);

  steps.verify = [] { return Roe<void>(Error("does not verify")); };
  EXPECT_EQ(ReceiveSigned(steps).verdict, SignedVerdict::Forged);
}

TEST(SignedReceiveTest, AnUnderstoodCriticalOrUnknownNonCriticalEntryIsAccepted) {
  Recorder recorder;
  recorder.ext = Critical();
  const uint16_t understood[] = {0x8001};
  SignedReceiveSteps steps = recorder.Steps(2);
  steps.understood_critical = understood;
  EXPECT_TRUE(ReceiveSigned(steps).accepted());

  recorder.ext = ExtArea::FromEntries({{.type = 0x0005, .value = {}}}).value();
  steps.understood_critical = {};
  EXPECT_TRUE(ReceiveSigned(steps).accepted());
}

TEST(SignedReceiveTest, NoAlgorithmsStepAndNoAreaAreAllowed) {
  SignedReceiveSteps steps{.what = "o", .outside_version = 2, .versions = kOnlyTwo, .parse = Pass, .verify = Pass};
  EXPECT_TRUE(ReceiveSigned(steps).accepted());
}

TEST(SignedReceiveTest, VerdictNamesAreStable) {
  EXPECT_EQ(SignedVerdictName(SignedVerdict::Accepted), "accepted");
  EXPECT_EQ(SignedVerdictName(SignedVerdict::Older), "older");
  EXPECT_EQ(SignedVerdictName(SignedVerdict::NeedsUpdate), "needs_update");
  EXPECT_EQ(SignedVerdictName(SignedVerdict::Malformed), "malformed");
  EXPECT_EQ(SignedVerdictName(SignedVerdict::Forged), "forged");
}

TEST(SignedReceiveTest, OutsideVersionsOfJsonAndBlobs) {
  Object object;
  EXPECT_EQ(OutsideVersionOfJson(object, "grant_version"), std::nullopt);
  object.set("grant_version", std::string("2"));
  EXPECT_EQ(OutsideVersionOfJson(object, "grant_version"), std::nullopt);
  object.set("grant_version", int64_t{2});
  EXPECT_EQ(OutsideVersionOfJson(object, "grant_version"), 2);
  object.set("grant_version", uint64_t{3});
  EXPECT_EQ(OutsideVersionOfJson(object, "grant_version"), 3);
  EXPECT_EQ(OutsideVersionOfJson(pp::common::TryParseObject(R"({"v": 4})").value(), "v"), 4);

  EXPECT_EQ(OutsideVersionOfBlob(ByteVector{2, 0}), 2);
  EXPECT_EQ(OutsideVersionOfBlob(ByteVector{}), std::nullopt);
}

// RC020-VEC: pp-cpp-common's unknown-critical cases, as step 5 of the order reads them (after a signature verified).
TEST(SignedReceiveTest, TheLibrarysUnknownCriticalCasesAreUpdateNeededAfterVerifying) {
  const Object vectors = test::VectorFile{PP_EXT_AREA_VECTORS_PATH, "PP_WRITE_EXT_AREA_VECTORS"}.Read();
  const pp::common::Array* cases = vectors.getArray("cases");
  const pp::common::Array* unknown = vectors.getArray("unknown_critical");
  ASSERT_NE(cases, nullptr);
  ASSERT_NE(unknown, nullptr);
  for (const pp::common::Value& item : unknown->elements) {
    const Object* row = pp::common::asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("case").value_or("");
    Object carrier;
    for (const pp::common::Value& c : cases->elements) {
      const Object* area = pp::common::asObject(c);
      if (area != nullptr && area->getString("name").value_or("") == name) {
        carrier.set(kExtB64Key, area->getString("ext_b64").value_or(""));
      }
    }
    const ExtArea ext = ReadExtB64(carrier).value();
    std::vector<uint16_t> understood;
    for (const pp::common::Value& type : row->getArray("understood")->elements) {
      understood.push_back(static_cast<uint16_t>(std::get<int64_t>(type)));
    }
    SignedReceiveSteps steps{.what = "o",
                             .outside_version = 2,
                             .versions = kOnlyTwo,
                             .parse = Pass,
                             .verify = Pass,
                             .ext = &ext,
                             .understood_critical = understood};
    EXPECT_EQ(ReceiveSigned(steps).needs_update(), !row->isNull("first_unknown_critical")) << name;
  }
}

// ---- One signed JSON object, end to end: how W1–W6 use the helpers. ----

constexpr char kNoteDomain[] = "pp-browser:test-note";
constexpr uint8_t kNoteVersion = 2;
constexpr SignedVersions kNoteVersions{.floor = kNoteVersion, .current = kNoteVersion};

ByteVector NoteSignBytes(const uint8_t version, const std::string& text, const ByteVector& typed_key,
                         const ExtArea& ext) {
  return SignedBytesWriter(kNoteDomain, version).Utf8(text).Typed(typed_key).Finish(ext);
}

Object WriteNote(const MlDsaKeyPair& keys, const std::string& text, const ExtArea& ext) {
  const ByteVector typed_key = TypedSigningPublicKey(keys.public_key).value();
  Object note;
  note.set("note_version", int64_t{kNoteVersion});
  note.set("text", text);
  note.set("public_key", Base64Encode(typed_key));
  WriteExtB64(note, ext);
  note.set("signature",
           Base64Encode(SignTyped(keys.secret_key, NoteSignBytes(kNoteVersion, text, typed_key, ext)).value()));
  return note;
}

/** The receiver: every object's reader has this shape. */
SignedReceipt ReadNote(const Object& note) {
  std::string text;
  ExtArea ext;
  TypedRead key;
  TypedRead signature;
  const std::optional<int64_t> version = OutsideVersionOfJson(note, "note_version");
  return ReceiveSigned({
      .what = "test note",
      .outside_version = version,
      .versions = kNoteVersions,
      .parse = [&]() -> Roe<void> {
        const std::optional<std::string> field = note.getString("text");
        if (!field) {
          return Error("no text");
        }
        text = *field;
        auto area = ReadExtB64(note);
        if (!area) {
          return area.error();
        }
        ext = std::move(area.value());
        return {};
      },
      .algorithms = [&]() -> Roe<void> {
        key = ReadTypedBlobB64(pp::BlobKind::kSignaturePublicKey, note.getString("public_key").value_or(""),
                               "public_key");
        if (!key.ok()) {
          return key.ToError();
        }
        signature = ReadTypedBlobB64(pp::BlobKind::kSignature, note.getString("signature").value_or(""), "signature");
        if (!signature.ok()) {
          return signature.ToError();
        }
        return {};
      },
      .verify =
          [&] {
            const ByteVector typed_key = TypedSigningPublicKey(key.raw).value();
            return VerifyTyped(key, NoteSignBytes(static_cast<uint8_t>(*version), text, typed_key, ext), signature);
          },
      .ext = &ext,
  });
}

MlDsaKeyPair NoteKeys() {
  return MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, 0x11)).value();
}

TEST(SignedReceiveTest, ASignedObjectIsAcceptedWithAnEmptyOrUnknownNonCriticalArea) {
  const MlDsaKeyPair keys = NoteKeys();
  const Object plain = WriteNote(keys, "hello", ExtArea{});
  EXPECT_FALSE(plain.contains(kExtB64Key));
  EXPECT_TRUE(ReadNote(plain).accepted()) << ReadNote(plain).detail;

  const Object extended = WriteNote(keys, "hello", ExtArea::FromEntries({{.type = 0x0042, .value = {1}}}).value());
  EXPECT_TRUE(ReadNote(extended).accepted()) << ReadNote(extended).detail;
}

TEST(SignedReceiveTest, ASignedObjectsRefusalsFollowTheOrder) {
  const MlDsaKeyPair keys = NoteKeys();
  const Object note = WriteNote(keys, "hello", ExtArea{});

  Object newer = note; // the outside version changed: update needed, never a forgery
  newer.set("note_version", int64_t{3});
  EXPECT_EQ(ReadNote(newer).verdict, SignedVerdict::NeedsUpdate);

  Object older = note;
  older.set("note_version", int64_t{1});
  EXPECT_EQ(ReadNote(older).verdict, SignedVerdict::Older);

  Object edited = note;
  edited.set("text", std::string("hellp"));
  EXPECT_EQ(ReadNote(edited).verdict, SignedVerdict::Forged);

  Object added_area = note; // an area the signer did not sign: the bytes change, so the signature fails
  added_area.set(kExtB64Key, std::string("gAEAAAACqrs=")); // one critical entry
  EXPECT_EQ(ReadNote(added_area).verdict, SignedVerdict::Forged);

  Object later_key = note;
  ByteVector raw = Base64Decode(note.getString("public_key").value()).value();
  raw[0] = 2;
  later_key.set("public_key", Base64Encode(raw));
  EXPECT_EQ(ReadNote(later_key).verdict, SignedVerdict::NeedsUpdate);

  Object bad_area = note;
  bad_area.set(kExtB64Key, std::string("AAAA"));
  EXPECT_EQ(ReadNote(bad_area).verdict, SignedVerdict::Malformed);

  const Object critical = WriteNote(keys, "hello", ExtArea::FromEntries({{.type = 0x8042, .value = {}}}).value());
  EXPECT_EQ(ReadNote(critical).verdict, SignedVerdict::NeedsUpdate);
}

} // namespace
} // namespace pbr
