// PeerRequestAuth v2's frozen vectors (release-compatibility SIGNED_FORMATS § 2.7, § 6): the signed bytes of a fixed
// history, blob and account-card request (the history one also with one unknown non-critical entry), and signed
// fixtures with the verdict the receiver's order gives each. The committed file is data/peer_request_vectors.json;
// PP_WRITE_PEER_REQUEST_VECTORS=<path> writes a fresh one (keys from fixed seeds; ML-DSA signatures are randomized,
// so only they differ between runs).

#include "lib/delivery/PeerRequestAuth.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using ::pp::kMlDsa65SeedBytes;
using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kFile{PP_PEER_REQUEST_VECTORS_PATH, "PP_WRITE_PEER_REQUEST_VECTORS"};
const std::set<std::string> kSignatureKeys{"signature"};

constexpr int64_t kNowMs = 1'761'235'200'000;
constexpr char kRequesterPeer[] = "QmRequesterDevice";

MlDsaKeyPair KeysOfSeed(const uint8_t fill) {
  return MlDsa::GenerateKeyPairFromSeed(ByteVector(kMlDsa65SeedBytes, fill)).value();
}

/** The requesting account (seed 0x51…) and another (0x52…) that forges. */
const MlDsaKeyPair& Requester() {
  static const MlDsaKeyPair keys = KeysOfSeed(0x51);
  return keys;
}
const MlDsaKeyPair& Forger() {
  static const MlDsaKeyPair keys = KeysOfSeed(0x52);
  return keys;
}

const std::string& RequesterAccount() {
  static const std::string account = AccountIdFromMlDsaPublicKey(Requester().public_key).value();
  return account;
}

PeerRequestAuth::SignFn SignerOf(const MlDsaKeyPair& keys) {
  return [&keys](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
    auto signature = MlDsa::Sign(keys.secret_key, bytes);
    if (!signature) {
      return signature.error();
    }
    return Base64Encode(*signature);
  };
}

pp::ExtArea Area(const uint16_t type) {
  return pp::ExtArea::FromEntries({{.type = type, .value = {0x01, 0x02, 0x03}}}).value();
}

ChatHistoryRequest FixedHistory() {
  ChatHistoryRequest request;
  request.requester_identity_kind = "account";
  request.requester_identity_value = RequesterAccount();
  request.peer_identity_kind = "account";
  request.peer_identity_value = "account:vector-peer";
  request.channel = ThreadChannel::E2ePublic;
  request.session_epoch = 3;
  request.min_sender_seq = 10;
  request.max_sender_seq = 42;
  request.limit = 50;
  request.order = "asc";
  return request;
}

ChatBlobRequest FixedBlob() {
  ChatBlobRequest request;
  request.op = ChatBlobOp::Fetch;
  request.requester_identity_kind = "account";
  request.requester_identity_value = RequesterAccount();
  request.peer_identity_kind = "account";
  request.peer_identity_value = "account:vector-peer";
  request.thread_id = "thread-vector";
  request.content_hash_hex = std::string(64, 'a');
  request.channel = ThreadChannel::E2e;
  return request;
}

AccountCardRequest FixedCard() {
  return AccountCardRequest{.account_id = "account:vector-peer"};
}

/** `request` signed by `keys` at kNowMs, then given the area `ext` (re-signed unless `sign_after` is false). */
template <typename Request>
Request Signed(Request request, const MlDsaKeyPair& keys, const pp::ExtArea& ext = {}, const bool sign_after = true) {
  EXPECT_TRUE(PeerRequestAuth::Sign(request, RequesterAccount(), kRequesterPeer, kNowMs, SignerOf(keys)));
  request.ext = ext;
  if (sign_after && !ext.empty()) {
    auto bytes = PeerRequestAuth::SignBytes(request);
    EXPECT_TRUE(bytes);
    request.signature = SignTypedB64(SignerOf(keys), *bytes).value();
  }
  return request;
}

Object Json(const ChatHistoryRequest& request) {
  return ChatHistoryRequestToJson(request);
}
Object Json(const ChatBlobRequest& request) {
  return ChatBlobRequestToJson(request);
}
Object Json(const AccountCardRequest& request) {
  return pp::common::TryParseObject(EncodeAccountCardRequest(request)).value();
}

/** The object the auth fields ride: the request itself, or an account-card request's `auth`. */
Object WithAuthField(Object json, const std::string& key, Value value) {
  if (const Object* auth = json.getObject("auth")) {
    Object changed = *auth;
    changed.set(key, std::move(value));
    json.set("auth", changed);
    return json;
  }
  json.set(key, std::move(value));
  return json;
}

Object Row(const std::string& kind, const std::string& name, const Object& request) {
  Object row;
  row.set("kind", kind);
  row.set("name", name);
  row.set("request", request);
  return row;
}

template <typename Request>
Object SignBytesRow(const std::string& kind, const std::string& name, const Request& request) {
  Object row = Row(kind, name, Json(request));
  row.set("sign_bytes_hex", BytesToHex(PeerRequestAuth::SignBytes(request).value()));
  return row;
}

Object ReceiveRow(const std::string& kind, const std::string& name, const Object& request,
                  const SignedVerdict verdict) {
  Object row = Row(kind, name, request);
  row.set("verdict", std::string(SignedVerdictName(verdict)));
  return row;
}

Object BuildVectors() {
  // Sign bytes: each request signed (its signature is not compared: only the bytes are frozen).
  auto unsigned_of = [](auto request) {
    EXPECT_TRUE(PeerRequestAuth::Sign(request, RequesterAccount(), kRequesterPeer, kNowMs, SignerOf(Requester())));
    return request;
  };
  ChatHistoryRequest history_entry = unsigned_of(FixedHistory());
  history_entry.ext = Area(0x0101);
  std::vector<Value> sign_bytes{
      pp::common::ObjectValue(SignBytesRow("history", "empty_area", unsigned_of(FixedHistory()))),
      pp::common::ObjectValue(SignBytesRow("history", "one_unknown_non_critical_entry", history_entry)),
      pp::common::ObjectValue(SignBytesRow("blob", "empty_area", unsigned_of(FixedBlob()))),
      pp::common::ObjectValue(SignBytesRow("account-card", "empty_area", unsigned_of(FixedCard()))),
  };

  const Object history = Json(Signed(FixedHistory(), Requester()));
  const Object card = Json(Signed(FixedCard(), Requester()));
  const std::string unknown_signature = Base64Encode(ByteVector{0x02, 0x00, 0x01});
  std::vector<Value> receive{
      pp::common::ObjectValue(ReceiveRow("history", "signed", history, SignedVerdict::Accepted)),
      pp::common::ObjectValue(
          ReceiveRow("blob", "signed", Json(Signed(FixedBlob(), Requester())), SignedVerdict::Accepted)),
      pp::common::ObjectValue(ReceiveRow("account-card", "signed", card, SignedVerdict::Accepted)),
      pp::common::ObjectValue(ReceiveRow("history", "signed_with_an_unknown_non_critical_entry",
                                         Json(Signed(FixedHistory(), Requester(), Area(0x0101))),
                                         SignedVerdict::Accepted)),
      pp::common::ObjectValue(ReceiveRow("history", "version_plus_one",
                                         WithAuthField(history, "auth_version", Value(PeerRequestAuth::kVersion + 1)),
                                         SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveRow("account-card", "version_plus_one",
                                         WithAuthField(card, "auth_version", Value(PeerRequestAuth::kVersion + 1)),
                                         SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveRow("history", "version_minus_one",
                                         WithAuthField(history, "auth_version", Value(PeerRequestAuth::kVersion - 1)),
                                         SignedVerdict::Older)),
      pp::common::ObjectValue(ReceiveRow("history", "unknown_critical_entry",
                                         Json(Signed(FixedHistory(), Requester(), Area(0x8101))),
                                         SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveRow("history", "unknown_signature_algorithm",
                                         WithAuthField(history, "signature", Value(unknown_signature)),
                                         SignedVerdict::NeedsUpdate)),
      pp::common::ObjectValue(ReceiveRow("history", "signed_by_another_key", Json(Signed(FixedHistory(), Forger())),
                                         SignedVerdict::Forged)),
      pp::common::ObjectValue(ReceiveRow("history", "an_unsigned_critical_entry",
                                         Json(Signed(FixedHistory(), Requester(), Area(0x8101), false)),
                                         SignedVerdict::Forged)),
      pp::common::ObjectValue(ReceiveRow("account-card", "malformed_extension_area",
                                         WithAuthField(card, kExtB64Key, Value(std::string("AAAA"))),
                                         SignedVerdict::Malformed)),
  };

  Object vectors;
  vectors.set("format", "pp-browser peer request vectors v1");
  vectors.set("about",
              "PeerRequestAuth v2 (release-compatibility SIGNED_FORMATS 2.7): sign_bytes are frozen (hex); receive "
              "fixtures give the receiver's verdict (SIGNED_FORMATS 1.1). Signatures are randomized: compare without "
              "them.");
  vectors.set("domain", PeerRequestAuth::kDomain);
  vectors.set("auth_version", PeerRequestAuth::kVersion);
  vectors.set("timestamp_ms", kNowMs);
  vectors.set("requester_seed_hex", BytesToHex(ByteVector(kMlDsa65SeedBytes, 0x51)));
  vectors.set("requester_account_id", RequesterAccount());
  vectors.set("requester_public_key_b64", Base64Encode(TypedSigningPublicKey(Requester().public_key).value()));
  vectors.set("sign_bytes", pp::common::ArrayValue(std::move(sign_bytes)));
  vectors.set("receive", pp::common::ArrayValue(std::move(receive)));
  return vectors;
}

/** Decode `json` as a `kind` request and run steps 1–5; with `bytes`, also give its signed bytes. */
Roe<SignedVerdict> VerdictOf(const std::string& kind, const Object& json, const std::string& key_b64,
                             std::vector<uint8_t>* bytes = nullptr) {
  const PeerRequestAuth::SigningKeySource key = [key_b64]() -> Roe<std::string> { return key_b64; };
  const auto run = [&](const auto& request) -> Roe<SignedVerdict> {
    if (bytes) {
      auto built = PeerRequestAuth::SignBytes(request);
      if (!built) {
        return built.error();
      }
      *bytes = *built;
    }
    return PeerRequestAuth::Receive(request, key).verdict;
  };
  if (kind == "history") {
    auto request = ChatHistoryRequestFromJson(json);
    return request ? run(*request) : Roe<SignedVerdict>(request.error());
  }
  if (kind == "blob") {
    auto request = ChatBlobRequestFromJson(json);
    return request ? run(*request) : Roe<SignedVerdict>(request.error());
  }
  auto request = DecodeAccountCardRequest(pp::common::DumpJson(json));
  return request ? run(*request) : Roe<SignedVerdict>(request.error());
}

TEST(PeerRequestVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

TEST(PeerRequestVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kSignatureKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kSignatureKeys)))
      << kFile.RegenerateHint();
}

TEST(PeerRequestVectorsTest, TheCommittedSignBytesRebuildFromTheirJson) {
  const Object file = kFile.Read();
  const Array* cases = file.getArray("sign_bytes");
  ASSERT_TRUE(cases != nullptr);
  ASSERT_EQ(cases->elements.size(), 4u);
  for (const Value& item : cases->elements) {
    const Object& row = *pp::common::asObject(item);
    const std::string name = row.getString("kind").value_or("") + "/" + row.getString("name").value_or("");
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(VerdictOf(row.getString("kind").value_or(""), *row.getObject("request"), "", &bytes)) << name;
    EXPECT_EQ(BytesToHex(bytes), row.getString("sign_bytes_hex").value_or("")) << name;
    EXPECT_EQ(SignedBytesVersion(bytes), std::optional<uint8_t>(static_cast<uint8_t>(PeerRequestAuth::kVersion)))
        << name;
  }
}

TEST(PeerRequestVectorsTest, EachCommittedFixtureGetsItsVerdict) {
  const Object file = kFile.Read();
  const std::string key = file.getString("requester_public_key_b64").value_or(""); // held typed, as the card holds it
  const Array* cases = file.getArray("receive");
  ASSERT_TRUE(cases != nullptr);
  ASSERT_EQ(cases->elements.size(), 12u);
  for (const Value& item : cases->elements) {
    const Object& row = *pp::common::asObject(item);
    const std::string name = row.getString("kind").value_or("") + "/" + row.getString("name").value_or("");
    auto verdict = VerdictOf(row.getString("kind").value_or(""), *row.getObject("request"), key);
    ASSERT_TRUE(verdict) << name << ": " << verdict.error().message;
    EXPECT_EQ(std::string(SignedVerdictName(*verdict)), row.getString("verdict").value_or("")) << name;
  }
}

} // namespace
} // namespace pbr
