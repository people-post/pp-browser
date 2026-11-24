// The sealed link handshake (docs/contracts/LINK_DEVICE.md; multi-device M027): the offer and the reply, the receiver's
// order for each, the 4-digit code, the text forms, and the frozen vectors in data/link_handshake_vectors.json (an
// offer from fixed coins, its code, and committed replies the fixed key opens — encapsulation is randomized, so only
// the replies differ between runs). PP_WRITE_LINK_HANDSHAKE_VECTORS=<path> writes a fresh file.

#include "foundation/crypto/LinkHandshake.h"

#include "common/tests/VectorFile.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace pbr {
namespace {

using pp::ExtArea;
using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using test::VectorFile;
using test::WithoutKeys;

const VectorFile kFile{PP_LINK_HANDSHAKE_VECTORS_PATH, "PP_WRITE_LINK_HANDSHAKE_VECTORS"};
/** The fields that hold a (randomized) reply. */
const std::set<std::string> kRandomKeys{"reply_b64"};
constexpr int64_t kNow = 1'700'000'000'000;
constexpr uint8_t kUnknownAlgId = 0x7f;

LinkOfferKeys KeysFromCoins(const uint8_t fill) {
  auto pair = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, fill)).value();
  return LinkOfferKeys(std::move(pair.public_key), std::move(pair.private_key));
}

ByteVector OfferOf(const LinkOfferKeys& keys, const ExtArea& ext = {}) {
  return LinkOfferBytes(keys.public_key(), ext).value();
}

LinkBundle Bundle(const uint8_t seed_fill = 0x22, const int64_t expires_at_ms = kNow + kLinkWindowMs) {
  LinkBundle bundle;
  bundle.seed = ByteVector(32, seed_fill);
  bundle.expires_at_ms = expires_at_ms;
  return bundle;
}

RelayAddress Relay() {
  return RelayAddress{.origin = "https://relay.example.org/api/relay",
                      .relay_id = *RelayId::FromRaw(ByteVector(kRelayIdBytes, 0x33))};
}

ExtArea CriticalEntry() {
  return ExtArea::FromEntries({ExtArea::Entry{.type = 0x8101, .value = {1}}}).value();
}

bool Contains(const ByteVector& haystack, const ByteVector& needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

TEST(LinkHandshakeTest, AReplyOpensWithTheOneTimeKeyAndCarriesTheBundle) {
  auto keys = LinkOfferKeys::Generate();
  ASSERT_TRUE(keys) << keys.error().message;
  const ByteVector offer = OfferOf(*keys);
  ASSERT_EQ(offer.size(), 1u + 8u + 1185u + 8u) << "version, LenBytes(typed key), the empty area";
  LinkBundle bundle = Bundle();
  bundle.relay = Relay();
  auto reply = SealLinkReply(offer, bundle);
  ASSERT_TRUE(reply) << reply.error().message;
  auto opened = OpenLinkReply(offer, *keys, *reply, kNow + 1000);
  ASSERT_TRUE(opened) << opened.error().message;
  EXPECT_EQ(opened->seed, bundle.seed);
  EXPECT_EQ(opened->intent, LinkIntent::Join);
  EXPECT_EQ(opened->expires_at_ms, bundle.expires_at_ms);
  ASSERT_TRUE(opened->relay.has_value());
  EXPECT_EQ(*opened->relay, Relay());

  auto without_relay = SealLinkReply(offer, Bundle());
  ASSERT_TRUE(without_relay);
  auto plain = OpenLinkReply(offer, *keys, *without_relay, kNow);
  ASSERT_TRUE(plain) << plain.error().message;
  EXPECT_FALSE(plain->relay.has_value()) << "no relay: the deployment's";
}

TEST(LinkHandshakeTest, AnotherDevicesKeyDoesNotOpenIt) {
  const LinkOfferKeys ours = KeysFromCoins(0x41);
  const LinkOfferKeys theirs = KeysFromCoins(0x42);
  const ByteVector offer = OfferOf(ours);
  const ByteVector reply = SealLinkReply(offer, Bundle()).value();
  auto opened = OpenLinkReply(offer, theirs, reply, kNow);
  ASSERT_FALSE(opened);
  EXPECT_TRUE(IsLinkError(opened.error(), LinkError::NotForThisDevice)) << opened.error().message;
  // Nor does a reply to another offer (an older one of this device's, say): the AAD names the offer.
  const ByteVector other_offer = OfferOf(theirs);
  const ByteVector for_other = SealLinkReply(other_offer, Bundle()).value();
  auto crossed = OpenLinkReply(offer, ours, for_other, kNow);
  ASSERT_FALSE(crossed);
  EXPECT_TRUE(IsLinkError(crossed.error(), LinkError::NotForThisDevice));
}

TEST(LinkHandshakeTest, ATamperedReplyIsRefused) {
  const LinkOfferKeys keys = KeysFromCoins(0x43);
  const ByteVector offer = OfferOf(keys);
  const ByteVector reply = SealLinkReply(offer, Bundle()).value();
  // In the KEM ciphertext (after the version and its u64 length and algorithm id), and in the sealed bundle (its last
  // byte, the tag).
  for (const size_t at : {size_t{1 + 8 + 1 + 10}, reply.size() - 1}) {
    ByteVector tampered = reply;
    tampered[at] = static_cast<uint8_t>(tampered[at] ^ 0x01U);
    auto opened = OpenLinkReply(offer, keys, tampered, kNow);
    ASSERT_FALSE(opened) << "byte " << at;
    EXPECT_TRUE(IsLinkError(opened.error(), LinkError::NotForThisDevice)) << opened.error().message;
  }
  ByteVector truncated(reply.begin(), reply.end() - 1);
  auto short_one = OpenLinkReply(offer, keys, truncated, kNow);
  ASSERT_FALSE(short_one);
  EXPECT_TRUE(IsLinkError(short_one.error(), LinkError::Unreadable)) << short_one.error().message;
}

TEST(LinkHandshakeTest, AnExpiredBundleIsRefused) {
  const LinkOfferKeys keys = KeysFromCoins(0x44);
  const ByteVector offer = OfferOf(keys);
  const ByteVector reply = SealLinkReply(offer, Bundle(0x22, kNow + 10)).value();
  EXPECT_TRUE(OpenLinkReply(offer, keys, reply, kNow + 9));
  auto late = OpenLinkReply(offer, keys, reply, kNow + 10);
  ASSERT_FALSE(late);
  EXPECT_TRUE(IsLinkError(late.error(), LinkError::Expired)) << late.error().message;
  // A key already wiped (its window passed on the new device) opens nothing.
  LinkOfferKeys gone = KeysFromCoins(0x44);
  gone.Wipe();
  auto wiped = OpenLinkReply(offer, gone, reply, kNow);
  ASSERT_FALSE(wiped);
  EXPECT_TRUE(IsLinkError(wiped.error(), LinkError::Expired));
}

// The receiver's order (SIGNED_EXTENSIONS): a newer version, algorithm, critical entry or intent is "update needed",
// never "not for this device" or a broken payload.
TEST(LinkHandshakeTest, ANewerOfferOrReplyNeedsAnUpdate) {
  const LinkOfferKeys keys = KeysFromCoins(0x45);
  ByteVector offer = OfferOf(keys);

  ByteVector newer_offer = offer;
  newer_offer[0] = 2;
  auto read = ReadLinkOffer(newer_offer);
  ASSERT_FALSE(read);
  EXPECT_TRUE(AppError::IsNewerFormat(read.error())) << read.error().message;

  ByteVector unknown_alg = offer;
  unknown_alg[1 + 8] = kUnknownAlgId; // the typed key's algorithm id
  auto alg = ReadLinkOffer(unknown_alg);
  ASSERT_FALSE(alg);
  EXPECT_TRUE(AppError::IsNewerFormat(alg.error())) << alg.error().message;

  auto critical = ReadLinkOffer(OfferOf(keys, CriticalEntry()));
  ASSERT_FALSE(critical);
  EXPECT_TRUE(AppError::IsNewerFormat(critical.error()));
  const ExtArea non_critical = ExtArea::FromEntries({ExtArea::Entry{.type = 0x0101, .value = {1}}}).value();
  EXPECT_TRUE(ReadLinkOffer(OfferOf(keys, non_critical))) << "an unknown non-critical entry is ignored";

  ByteVector unversioned = offer;
  unversioned[0] = 0;
  auto zero = ReadLinkOffer(unversioned);
  ASSERT_FALSE(zero);
  EXPECT_TRUE(IsLinkError(zero.error(), LinkError::Unreadable));

  ByteVector reply = SealLinkReply(offer, Bundle()).value();
  reply[0] = 2;
  auto newer_reply = OpenLinkReply(offer, keys, reply, kNow);
  ASSERT_FALSE(newer_reply);
  EXPECT_TRUE(AppError::IsNewerFormat(newer_reply.error())) << newer_reply.error().message;

  LinkBundle unknown_intent = Bundle();
  unknown_intent.intent = static_cast<LinkIntent>(2);
  auto intent = OpenLinkReply(offer, keys, SealLinkReply(offer, unknown_intent).value(), kNow);
  ASSERT_FALSE(intent);
  EXPECT_TRUE(AppError::IsNewerFormat(intent.error())) << intent.error().message;

  auto critical_bundle = OpenLinkReply(offer, keys, SealLinkReply(offer, Bundle(), CriticalEntry()).value(), kNow);
  ASSERT_FALSE(critical_bundle);
  EXPECT_TRUE(AppError::IsNewerFormat(critical_bundle.error()));
}

TEST(LinkHandshakeTest, BothDevicesShowTheSameCodeAndAnotherKeyChangesIt) {
  const LinkOfferKeys keys = KeysFromCoins(0x46);
  const ByteVector offer = OfferOf(keys);
  const std::string code = LinkConfirmationCode(offer);
  ASSERT_EQ(code.size(), 4u);
  EXPECT_TRUE(std::all_of(code.begin(), code.end(), [](const char c) { return c >= '0' && c <= '9'; }));
  // The main device reads the offer from the QR code or the text: the same bytes, the same digits.
  auto pasted = FindLinkPayload(kLinkOfferTextPrefix, "here: " + LinkPayloadText(kLinkOfferTextPrefix, offer));
  ASSERT_TRUE(pasted);
  EXPECT_EQ(LinkConfirmationCode(*pasted), code);
  EXPECT_NE(LinkConfirmationCode(OfferOf(KeysFromCoins(0x47))), code) << "a swapped offer shows other digits";
}

TEST(LinkHandshakeTest, NoSecretIsInEitherPayload) {
  const LinkOfferKeys keys = KeysFromCoins(0x48);
  const ByteVector offer = OfferOf(keys);
  const LinkBundle bundle = Bundle(0x5a);
  const ByteVector reply = SealLinkReply(offer, bundle).value();
  const ByteVector seed_part(bundle.seed.begin(), bundle.seed.begin() + 8);
  const ByteVector secret_part(keys.secret_key().begin(), keys.secret_key().begin() + 16);
  for (const ByteVector* payload : {&offer, &reply}) {
    EXPECT_FALSE(Contains(*payload, seed_part));
    EXPECT_FALSE(Contains(*payload, secret_part));
    const std::string text = LinkPayloadText(payload == &offer ? kLinkOfferTextPrefix : kLinkReplyTextPrefix, *payload);
    EXPECT_EQ(text.find(Base64UrlEncode(bundle.seed).substr(0, 12)), std::string::npos);
    EXPECT_EQ(text.find(Base64Encode(bundle.seed).substr(0, 12)), std::string::npos);
  }
}

TEST(LinkHandshakeTest, ThePayloadIsFoundInScannedBytesOrPastedText) {
  const LinkOfferKeys keys = KeysFromCoins(0x49);
  const ByteVector offer = OfferOf(keys);
  const std::string scanned(offer.begin(), offer.end());
  EXPECT_EQ(FindLinkPayload(kLinkOfferTextPrefix, scanned), offer) << "a QR code's binary bytes, as they are";
  const std::string text = LinkPayloadText(kLinkOfferTextPrefix, offer);
  EXPECT_EQ(FindLinkPayload(kLinkOfferTextPrefix, "Link my phone:\n" + text + "\nthanks"), offer);
  EXPECT_FALSE(FindLinkPayload(kLinkReplyTextPrefix, text)) << "an offer is not a reply";
  EXPECT_FALSE(FindLinkPayload(kLinkOfferTextPrefix, "pp-browser-contact-v1:eyJhY2NvdW50X2lkIjoiYSJ9"))
      << "a QR code of text is not read as a payload of some newer version";
  EXPECT_FALSE(FindLinkPayload(kLinkOfferTextPrefix, ""));
  auto garbage = ReadLinkOffer(*FindLinkPayload(kLinkOfferTextPrefix, std::string("\x01\x02\x03", 3)));
  ASSERT_FALSE(garbage);
  EXPECT_TRUE(IsLinkError(garbage.error(), LinkError::Unreadable));
}

TEST(LinkHandshakeTest, SealingRefusesABundleOutOfBounds) {
  const ByteVector offer = OfferOf(KeysFromCoins(0x4a));
  LinkBundle short_seed = Bundle();
  short_seed.seed.resize(31);
  EXPECT_FALSE(SealLinkReply(offer, short_seed));
  EXPECT_FALSE(SealLinkReply(offer, Bundle(0x22, 0))) << "no expiry";
  LinkBundle long_origin = Bundle();
  long_origin.relay = Relay();
  long_origin.relay->origin = "https://" + std::string(130, 'a');
  EXPECT_FALSE(SealLinkReply(offer, long_origin));
  EXPECT_FALSE(SealLinkReply(ByteVector{1, 2, 3}, Bundle())) << "not an offer";
}

// --- Frozen vectors ---

Object ReplyCase(const char* name, const ByteVector& offer, const LinkBundle& bundle) {
  Object row;
  row.set("name", name);
  row.set("seed_hex", BytesToHex(bundle.seed));
  row.set("intent", static_cast<int64_t>(bundle.intent));
  row.set("expires_at_ms", bundle.expires_at_ms);
  if (bundle.relay) {
    row.set("relay_origin", bundle.relay->origin);
    row.set("relay_id", bundle.relay->relay_id.ToString());
  }
  row.set("reply_b64", Base64Encode(SealLinkReply(offer, bundle).value()));
  return row;
}

Object BuildVectors() {
  const LinkOfferKeys keys = KeysFromCoins(0x61);
  const ByteVector offer = OfferOf(keys);
  Object root;
  root.set("format", "pp-browser link handshake vectors v1");
  root.set("about",
           "The link offer and sealed reply, version 1 (docs/contracts/LINK_DEVICE.md). The offer is frozen "
           "(the new device's one-time ML-KEM-768 key from kem_coins_hex), and so is its 4-digit code. "
           "Encapsulation is randomized: compare without reply_b64, and check each committed reply opens with "
           "that key to its bundle fields.");
  root.set("offer_version", static_cast<int64_t>(kLinkOfferVersion));
  root.set("reply_version", static_cast<int64_t>(kLinkReplyVersion));
  root.set("kem_coins_hex", BytesToHex(ByteVector(64, 0x61)));
  root.set("offer_b64", Base64Encode(offer));
  root.set("offer_text", LinkPayloadText(kLinkOfferTextPrefix, offer));
  root.set("confirmation_code", LinkConfirmationCode(offer));
  std::vector<Value> replies;
  LinkBundle plain = Bundle(0x62, kNow + kLinkWindowMs);
  replies.push_back(ObjectValue(ReplyCase("join", offer, plain)));
  LinkBundle relayed = Bundle(0x63, kNow + kLinkWindowMs);
  relayed.relay = Relay();
  replies.push_back(ObjectValue(ReplyCase("join_with_relay", offer, relayed)));
  root.set("replies", ArrayValue(std::move(replies)));
  return root;
}

TEST(LinkHandshakeVectorsTest, WriteAFreshFileWhenAsked) {
  if (!kFile.WriteRequested()) {
    GTEST_SKIP() << kFile.SkipReason();
  }
  ASSERT_TRUE(kFile.Write(BuildVectors()));
}

TEST(LinkHandshakeVectorsTest, TheCommittedFileIsWhatTheBuildersMake) {
  EXPECT_EQ(VectorFile::Text(WithoutKeys(kFile.Read(), kRandomKeys)),
            VectorFile::Text(WithoutKeys(BuildVectors(), kRandomKeys)))
      << kFile.RegenerateHint();
}

TEST(LinkHandshakeVectorsTest, EachCommittedReplyOpensToItsBundle) {
  const Object committed = kFile.Read();
  const LinkOfferKeys keys = KeysFromCoins(0x61);
  const ByteVector offer = Base64Decode(committed.getString("offer_b64").value_or("")).valueOr(ByteVector{});
  ASSERT_EQ(offer, OfferOf(keys));
  const Array* replies = committed.getArray("replies");
  ASSERT_NE(replies, nullptr);
  ASSERT_EQ(replies->elements.size(), 2u);
  for (const Value& item : replies->elements) {
    const Object* row = asObject(item);
    ASSERT_NE(row, nullptr);
    const std::string name = row->getString("name").value_or("");
    auto reply = Base64Decode(row->getString("reply_b64").value_or(""));
    ASSERT_TRUE(reply) << name;
    auto opened = OpenLinkReply(offer, keys, *reply, kNow);
    ASSERT_TRUE(opened) << name << ": " << opened.error().message;
    EXPECT_EQ(BytesToHex(opened->seed), row->getString("seed_hex").value_or("")) << name;
    EXPECT_EQ(opened->expires_at_ms, row->getIf<int64_t>("expires_at_ms").value_or(0)) << name;
    EXPECT_EQ(opened->relay ? opened->relay->origin : std::string(), row->getString("relay_origin").value_or(""))
        << name;
    EXPECT_EQ(opened->relay ? opened->relay->relay_id.ToString() : std::string(),
              row->getString("relay_id").value_or(""))
        << name;
  }
}

} // namespace
} // namespace pbr
