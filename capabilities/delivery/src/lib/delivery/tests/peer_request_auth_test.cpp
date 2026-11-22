// PeerRequestAuth v2 (release-compatibility SIGNED_FORMATS § 2.7): the receiver's order through Verify — a request
// this app cannot read is "update needed" (the server answers update_needed), every other refusal one plain error —
// and the request's own checks after it (its link, its time). The frozen bytes are peer_request_vectors_test's.

#include "lib/delivery/PeerRequestAuth.h"

#include "common/ValueJson.h"
#include "common/chat/MessagingJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/error/AppError.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "lib/people/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

pp::ExtArea Area(const uint16_t type) {
  return pp::ExtArea::FromEntries({{.type = type, .value = {7}}}).value();
}

const test::TestAccount& Asker() {
  static const test::TestAccount account = test::TestAccount::Make();
  return account;
}

PeerRequestAuth::SignFn SignerOf(const test::TestAccount& account) {
  return [&account](const std::vector<uint8_t>& bytes) { return account.Sign(bytes); };
}

/** A key store that knows the asker's key. */
struct Keys {
  PeerSigningKeyStore store;
  PeerSigningKeyResolver resolver{store};
  Keys() {
    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = Asker().TypedSigningPublicB64();
    record.source = "test";
    EXPECT_TRUE(store.Put("account", Asker().account_id, record));
  }
};

ChatHistoryRequest SignedHistory(const pp::ExtArea& ext = {}) {
  ChatHistoryRequest request{.requester_identity_kind = "account",
                             .requester_identity_value = Asker().account_id,
                             .peer_identity_kind = "account",
                             .peer_identity_value = "account:me"};
  EXPECT_TRUE(PeerRequestAuth::Sign(request, Asker().account_id, "QmAsker", kNow, SignerOf(Asker())));
  if (!ext.empty()) {
    request.ext = ext;
    request.signature = SignTypedB64(SignerOf(Asker()), PeerRequestAuth::SignBytes(request).value()).value();
  }
  return request;
}

TEST(PeerRequestAuthTest, SigningWritesVersionTwoAndATypedSignature) {
  const ChatHistoryRequest request = SignedHistory();
  EXPECT_EQ(request.auth_version, std::optional<int64_t>(2));
  auto bytes = PeerRequestAuth::SignBytes(request);
  ASSERT_TRUE(bytes);
  const std::string domain = "pp-browser:peer-request";
  ASSERT_GT(bytes->size(), domain.size() + 2);
  EXPECT_EQ(std::string(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(domain.size())), domain);
  EXPECT_EQ((*bytes)[domain.size()], 0u);
  EXPECT_EQ((*bytes)[domain.size() + 1], 2u) << "the version after the domain";
  auto signature = Base64Decode(request.signature);
  ASSERT_TRUE(signature);
  EXPECT_EQ(signature->size(), 3310u) << "ml-dsa-65, typed";
  EXPECT_EQ(signature->front(), 1u);
}

// Accepted, then the request's own checks: the link it signed for and its time (step 6).
TEST(PeerRequestAuthTest, AVerifiedRequestIsServedOnlyOnItsLinkAndInItsTime) {
  Keys keys;
  const ChatHistoryRequest request = SignedHistory();
  EXPECT_TRUE(PeerRequestAuth::Verify(request, "QmAsker", kNow, keys.resolver));
  EXPECT_FALSE(PeerRequestAuth::Verify(request, "QmOther", kNow, keys.resolver)) << "another link";
  EXPECT_FALSE(PeerRequestAuth::Verify(request, "QmAsker", kNow + PeerRequestAuth::kMaxSkewMs + 1, keys.resolver));
  PeerSigningKeyStore none;
  PeerSigningKeyResolver no_keys{none};
  EXPECT_FALSE(PeerRequestAuth::Verify(request, "QmAsker", kNow, no_keys)) << "no key for the asker";
}

// RC020-ORDER step 1: a newer auth_version is "update needed" — before anything is verified, so even unsigned.
TEST(PeerRequestAuthTest, ANewerVersionIsUpdateNeededBeforeVerifying) {
  Keys keys;
  ChatHistoryRequest request = SignedHistory();
  request.auth_version = 3;
  request.signature = "not a signature";
  auto verified = PeerRequestAuth::Verify(request, "QmAsker", kNow, keys.resolver);
  ASSERT_FALSE(verified);
  EXPECT_TRUE(AppError::IsNewerFormat(verified.error()));
}

// Step 1: missing or older is an older app's — refused, never "update needed".
TEST(PeerRequestAuthTest, AMissingOrOlderVersionIsRefused) {
  Keys keys;
  for (const std::optional<int64_t> version : {std::optional<int64_t>{}, std::optional<int64_t>{1}}) {
    ChatHistoryRequest request = SignedHistory();
    request.auth_version = version;
    auto verified = PeerRequestAuth::Verify(request, "QmAsker", kNow, keys.resolver);
    ASSERT_FALSE(verified);
    EXPECT_FALSE(AppError::IsNewerFormat(verified.error()));
    EXPECT_EQ(
        PeerRequestAuth::Receive(request, [] { return Roe<std::string>(Asker().TypedSigningPublicB64()); }).verdict,
        SignedVerdict::Older);
  }
}

// Step 2: a bad area, read as the request arrived, is malformed — after the version.
TEST(PeerRequestAuthTest, AMalformedAreaIsRefusedAfterTheVersion) {
  Object json = ChatHistoryRequestToJson(SignedHistory());
  json.set(kExtB64Key, "AAAA");
  auto request = ChatHistoryRequestFromJson(json);
  ASSERT_TRUE(request);
  EXPECT_FALSE(request->ext_error.empty());
  const PeerRequestAuth::SigningKeySource key = [] { return Roe<std::string>(Asker().TypedSigningPublicB64()); };
  EXPECT_EQ(PeerRequestAuth::Receive(*request, key).verdict, SignedVerdict::Malformed);
  request->auth_version = 3;
  EXPECT_EQ(PeerRequestAuth::Receive(*request, key).verdict, SignedVerdict::NeedsUpdate) << "the version comes first";
}

// Step 3: an unknown signature algorithm is "update needed".
TEST(PeerRequestAuthTest, AnUnknownAlgorithmIsUpdateNeeded) {
  Keys keys;
  ChatHistoryRequest request = SignedHistory();
  request.signature = Base64Encode(ByteVector{0x02, 0x00});
  auto verified = PeerRequestAuth::Verify(request, "QmAsker", kNow, keys.resolver);
  ASSERT_FALSE(verified);
  EXPECT_TRUE(AppError::IsNewerFormat(verified.error()));
}

// Step 4: a changed field, another signer — a forgery, not "update needed".
TEST(PeerRequestAuthTest, ARequestThatDoesNotVerifyIsRefused) {
  Keys keys;
  ChatHistoryRequest changed = SignedHistory();
  changed.session_epoch = 9;
  auto verified = PeerRequestAuth::Verify(changed, "QmAsker", kNow, keys.resolver);
  ASSERT_FALSE(verified);
  EXPECT_FALSE(AppError::IsNewerFormat(verified.error()));
  const PeerRequestAuth::SigningKeySource key = [] { return Roe<std::string>(Asker().TypedSigningPublicB64()); };
  EXPECT_EQ(PeerRequestAuth::Receive(changed, key).verdict, SignedVerdict::Forged);
}

// Step 5: an unknown critical entry is "update needed" only once the signature verified; a non-critical one is kept.
TEST(PeerRequestAuthTest, ExtensionsAreReadAfterTheSignature) {
  Keys keys;
  const ChatHistoryRequest critical = SignedHistory(Area(0x8001));
  auto verified = PeerRequestAuth::Verify(critical, "QmAsker", kNow, keys.resolver);
  ASSERT_FALSE(verified);
  EXPECT_TRUE(AppError::IsNewerFormat(verified.error()));

  ChatHistoryRequest unsigned_critical = SignedHistory();
  unsigned_critical.ext = Area(0x8001); // added after signing
  verified = PeerRequestAuth::Verify(unsigned_critical, "QmAsker", kNow, keys.resolver);
  ASSERT_FALSE(verified);
  EXPECT_FALSE(AppError::IsNewerFormat(verified.error())) << "an unverified request never says update needed";

  const ChatHistoryRequest non_critical = SignedHistory(Area(0x0001));
  EXPECT_TRUE(PeerRequestAuth::Verify(non_critical, "QmAsker", kNow, keys.resolver));
  auto round_trip = ChatHistoryRequestFromJson(ChatHistoryRequestToJson(non_critical));
  ASSERT_TRUE(round_trip);
  EXPECT_EQ(round_trip->ext, non_critical.ext) << "kept as received";
  EXPECT_TRUE(PeerRequestAuth::Verify(*round_trip, "QmAsker", kNow, keys.resolver));
}

TEST(PeerRequestAuthTest, BlobRequestsReadTheSameWay) {
  Keys keys;
  ChatBlobRequest request{.requester_identity_kind = "account",
                          .requester_identity_value = Asker().account_id,
                          .peer_identity_kind = "account",
                          .peer_identity_value = "account:me",
                          .thread_id = "t",
                          .content_hash_hex = std::string(64, 'b')};
  ASSERT_TRUE(PeerRequestAuth::Sign(request, Asker().account_id, "QmAsker", kNow, SignerOf(Asker())));
  auto round_trip = ChatBlobRequestFromJson(ChatBlobRequestToJson(request));
  ASSERT_TRUE(round_trip);
  EXPECT_EQ(round_trip->auth_version, std::optional<int64_t>(2));
  EXPECT_TRUE(PeerRequestAuth::Verify(*round_trip, "QmAsker", kNow, keys.resolver));
  round_trip->auth_version = 3;
  auto verified = PeerRequestAuth::Verify(*round_trip, "QmAsker", kNow, keys.resolver);
  ASSERT_FALSE(verified);
  EXPECT_TRUE(AppError::IsNewerFormat(verified.error()));
}

TEST(PeerRequestAuthTest, TheUpdateNeededRefusalIsCoded) {
  const Object refusal = UpdateNeededRefusalToJson();
  EXPECT_EQ(pp::common::DumpJson(refusal), R"({"ok":false,"code":"update_needed"})");
  EXPECT_TRUE(IsUpdateNeededRefusal(refusal));
  EXPECT_FALSE(IsUpdateNeededRefusal(ChatBlobAckToJson(false, "Not available")));
}

TEST(AccountCardRequestAuthTest, TheRequestersOwnKeyCardStandsInForALookup) {
  const test::TestAccount asker = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  AccountCardRequest request{.account_id = "account:someone", .requester_keys = asker.Card()};
  ASSERT_TRUE(PeerRequestAuth::Sign(request, asker.account_id, "QmAsker", kNow,
                                    [&](const std::vector<uint8_t>& bytes) { return asker.Sign(bytes); }));
  PeerSigningKeyStore none;
  PeerSigningKeyResolver no_keys{none};
  EXPECT_TRUE(PeerRequestAuth::Verify(request, "QmAsker", kNow, no_keys)) << "checked with the card it carries";
  EXPECT_FALSE(PeerRequestAuth::Verify(request, "QmOther", kNow, no_keys)) << "another link";
  AccountCardRequest swapped = request;
  swapped.requester_keys = mallory.Card(); // someone else's card: does not prove itself for the asker
  EXPECT_FALSE(PeerRequestAuth::Verify(swapped, "QmAsker", kNow, no_keys));
  AccountCardRequest retargeted = request;
  retargeted.account_id = "account:another";
  EXPECT_FALSE(PeerRequestAuth::Verify(retargeted, "QmAsker", kNow, no_keys)) << "the signature covers whose card";
}

// The auth object round-trips with its version and area; a newer version reads as such, and the answer to it is coded.
TEST(AccountCardRequestAuthTest, TheAuthObjectCarriesItsVersionAndArea) {
  const test::TestAccount asker = test::TestAccount::Make();
  AccountCardRequest request{.account_id = "account:someone", .requester_keys = asker.Card()};
  ASSERT_TRUE(PeerRequestAuth::Sign(request, asker.account_id, "QmAsker", kNow,
                                    [&](const std::vector<uint8_t>& bytes) { return asker.Sign(bytes); }));
  auto decoded = DecodeAccountCardRequest(EncodeAccountCardRequest(request));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->auth_version, std::optional<int64_t>(2));
  PeerSigningKeyStore none;
  PeerSigningKeyResolver no_keys{none};
  EXPECT_TRUE(PeerRequestAuth::Verify(*decoded, "QmAsker", kNow, no_keys));
  decoded->auth_version = 3;
  auto verified = PeerRequestAuth::Verify(*decoded, "QmAsker", kNow, no_keys);
  ASSERT_FALSE(verified);
  EXPECT_TRUE(AppError::IsNewerFormat(verified.error()));

  auto reply = DecodeAccountCardReply(EncodeAccountCardReply(AccountCardReply{.error = kAccountCardUpdateNeeded}));
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->error, kAccountCardUpdateNeeded);
  EXPECT_EQ(EncodeAccountCardReply(AccountCardReply{.error = kAccountCardUpdateNeeded}),
            R"({"ok":false,"code":"update_needed"})");
}

} // namespace
} // namespace pbr
