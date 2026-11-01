// Cross-implementation vectors (projects/social-posts): entries, a head, a media presign and reports signed by this library, which
// other implementations (the app-posts relay) must parse, hash and verify alike. The committed file is
// checked here; PP_WRITE_POST_VECTORS=<path> writes a fresh one (signatures are randomized, keys are not).

#include "lib/posts/PostBodies.h"
#include "lib/posts/PostEntry.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/KemKeyBinding.h"
#include "foundation/crypto/MlDsa.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int64_t kT0 = 1'761'235'200'000;

std::string Hex(const PostHash& hash) {
  return BytesToHex(ByteVector(hash.begin(), hash.end()));
}

PostHash HashOfText(const std::string& text) {
  const ByteVector digest = Blake2b256(ByteVector(text.begin(), text.end()));
  PostHash hash{};
  std::copy(digest.begin(), digest.end(), hash.begin());
  return hash;
}

Object BuildVectors() {
  const auto keys = MlDsa::GenerateKeyPairFromSeed(ByteVector(32, 0x01)).value();
  const auto kem = HybridKem::GenerateKeyPairFromSeed(ByteVector(64, 0x02)).value();
  const std::string account = AccountIdFromMlDsaPublicKey(keys.public_key).value();
  const std::string signing_b64 = Base64Encode(keys.public_key);
  const std::string kem_b64 = Base64Encode(kem.public_key);
  const PostSigner signer = [&](const ByteVector& message) { return MlDsa::Sign(keys.secret_key, message); };

  Object key_card;
  key_card.set("account_id", account);
  key_card.set("signing_public_key_b64", signing_b64);
  key_card.set("kem_public_key_b64", kem_b64);
  key_card.set("kem_binding_signature_b64", Base64Encode(signer(KemBindingSignBytes(signing_b64, kem_b64)).value()));

  const PostLane lane{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};
  std::vector<Value> entries;
  PostHash prev{};
  PostRef first{};
  uint64_t seq = 0;
  auto add = [&](uint8_t kind, const ByteVector& body, const char* note, std::vector<PostExtension> extensions = {}) {
    ++seq;
    const PostEntry entry{.author = account, .lane = lane, .seq = seq, .prev = prev,
                          .claimed_at = kT0 + static_cast<int64_t>(seq) * 1000, .kind = kind, .body = body,
                          .extensions = std::move(extensions)};
    const SignedPostEntry signed_entry = SignPostEntry(entry, signer).value();
    const PostHash hash = PostEntryHash(signed_entry.canonical);
    if (seq == 1) {
      first = PostRefOf(entry, hash);
    }
    Object row;
    row.set("note", std::string(note));
    row.set("seq", static_cast<int64_t>(seq));
    row.set("kind", static_cast<int64_t>(kind));
    row.set("claimed_at", entry.claimed_at);
    row.set("signed_b64", Base64Encode(SerializeSignedPostEntry(signed_entry)));
    row.set("entry_hash_hex", Hex(hash));
    entries.push_back(ObjectValue(std::move(row)));
    prev = hash;
  };
  // "hello @Vector #你好 https://pp.example": a mention, a hashtag and a link (byte spans, S026).
  const std::string text = "hello @Vector #\xE4\xBD\xA0\xE5\xA5\xBD https://pp.example";
  add(1, EncodePostBody({.text = text,
                         .media = {{.content_id = HashOfText("media"), .mime = "image/png", .size = 1234,
                                    .alt = "a test image", .width = 640, .height = 480,
                                    .placeholder = ByteVector{0x1f, 0x2e, 0x3d}}},
                         .facets = {{.start = 6, .end = 13, .type = 1, .value = account},
                                    {.start = 14, .end = 21, .type = 3, .value = "\xE4\xBD\xA0\xE5\xA5\xBD"},
                                    {.start = 22, .end = 40, .type = 2, .value = "https://pp.example"}},
                         .content_warning = "test content",
                         .labels = {"graphic"},
                         .languages = {"en", "zh-Hans"}}),
      "post with text, one image, facets, a content warning, a label and languages");
  add(1, EncodePostBody({.text = "a reply quoting the first", .reply = PostReply{.root = first, .parent = first},
                         .quote = first}),
      "a reply (thread root and parent) that also quotes");
  add(4, EncodeReactBody({.target = first, .reaction = "like"}), "a like of the first post");
  add(2, EncodeEditBody({.target = first, .replacement = {.text = "hello again"}}), "an edit of the first post");
  add(5, EncodeTargetBody({.target = first}), "a repost");
  add(6, EncodeProfileBody({.display_name = "Vector", .bio = "test vectors", .avatar = HashOfText("avatar"),
                            .services = {{.kind = "post_relay", .terms = "{\"entries_per_day\":100}"}}}),
      "a profile offering a service");
  add(8, EncodeDisavowBody({.targets = {first}}), "a disavowal");
  add(99, ByteVector{0xde, 0xad}, "an unknown kind: valid, kept as a stub");
  add(1, EncodePostBody({.text = "with extensions"}), "a post with two extensions a reader skips",
      {{.tag = 1, .value = ByteVector{0x01, 0x02}}, {.tag = 513, .value = {}}});
  add(3, EncodeTargetBody({.target = first}), "a delete of the first post");
  add(7, EncodeLaneCloseBody({.lane = lane, .last_seq = seq + 1}), "the lane closes");

  const PostHead head{.author = account,
                      .issued_at = kT0 + 100'000,
                      .lanes = {{.lane = lane, .seq = seq, .entry_hash = prev}},
                      .targets = {"https://relay.example/api/relay"}};
  const SignedPostHead signed_head = SignPostHead(head, signer).value();
  ByteVector head_bytes = signed_head.canonical;
  head_bytes.insert(head_bytes.end(), signed_head.signature.begin(), signed_head.signature.end());
  Object head_json;
  head_json.set("signed_b64", Base64Encode(head_bytes));
  head_json.set("head_hash_hex", Hex(PostHeadHash(signed_head.canonical)));

  const PostHash media_id = HashOfText("media");
  const ByteVector media_bytes = PostMediaPresignSignBytes(account, media_id, 1234, "image/png", kT0 + 200'000);
  Object media;
  media.set("content_id_hex", Hex(media_id));
  media.set("size", static_cast<int64_t>(1234));
  media.set("mime", std::string("image/png"));
  media.set("issued_at", kT0 + 200'000);
  media.set("sign_bytes_hex", BytesToHex(media_bytes));
  media.set("signature_b64", Base64Encode(signer(media_bytes).value()));

  // Reports (S025): an entry report and a media report, both signed by the vector account.
  std::vector<Value> reports;
  auto add_report = [&](const char* subject, const ByteVector& bytes, const std::string& reason, const std::string& note) {
    Object report;
    report.set("subject", std::string(subject));
    report.set("reason", reason);
    report.set("note", note);
    report.set("issued_at", kT0 + 300'000);
    report.set("sign_bytes_hex", BytesToHex(bytes));
    report.set("signature_b64", Base64Encode(signer(bytes).value()));
    reports.push_back(ObjectValue(std::move(report)));
  };
  add_report("entry", PostEntryReportSignBytes(account, first, "spam", "test report", kT0 + 300'000), "spam",
             "test report");
  add_report("media", PostMediaReportSignBytes(account, media_id, "illegal", "", kT0 + 300'000), "illegal", "");

  Object root;
  root.set("format", std::string("pp-browser post vectors v1"));
  root.set("media_presign", ObjectValue(std::move(media)));
  root.set("first_entry_hash_hex", Hex(first.entry_hash));
  root.set("reports", ArrayValue(std::move(reports)));
  root.set("account_id", account);
  root.set("key_card", ObjectValue(std::move(key_card)));
  root.set("lane_hex", BytesToHex(ByteVector(lane.begin(), lane.end())));
  root.set("entries", ArrayValue(std::move(entries)));
  root.set("head", ObjectValue(std::move(head_json)));
  return root;
}

TEST(PostVectorsTest, TheCommittedVectorsVerify) {
  if (const char* out = std::getenv("PP_WRITE_POST_VECTORS"); out && *out) {
    std::ofstream file(out);
    file << DumpJson(BuildVectors()) << "\n";
    ASSERT_TRUE(file.good());
    return;
  }
  std::ifstream file(PP_POST_VECTORS_PATH);
  ASSERT_TRUE(file.good()) << PP_POST_VECTORS_PATH;
  std::stringstream text;
  text << file.rdbuf();
  auto root = TryParseObject(text.str());
  ASSERT_TRUE(root);
  const Object* key_card = root->getObject("key_card");
  ASSERT_NE(key_card, nullptr);
  const ByteVector key = Base64Decode(key_card->getString("signing_public_key_b64").value_or("")).value();
  const std::string account = root->getString("account_id").value_or("");
  EXPECT_EQ(AccountIdFromMlDsaPublicKey(key).value(), account);

  const Array* entries = root->getArray("entries");
  ASSERT_NE(entries, nullptr);
  ASSERT_EQ(entries->elements.size(), 11u);
  PostEntry prev;
  PostHash prev_hash{};
  for (const Value& item : entries->elements) {
    const Object* row = asObject(item);
    ASSERT_NE(row, nullptr);
    const auto signed_entry =
        DeserializeSignedPostEntry(Base64Decode(row->getString("signed_b64").value_or("")).value()).value();
    auto entry = VerifyPostEntry(signed_entry, key, kT0 + 3'600'000);
    ASSERT_TRUE(entry) << row->getString("note").value_or("") << ": " << entry.error().message;
    const PostHash hash = PostEntryHash(signed_entry.canonical);
    EXPECT_EQ(Hex(hash), row->getString("entry_hash_hex").value_or(""));
    EXPECT_EQ(PostEntryCanonicalBytes(entry.value()), signed_entry.canonical) << "one canonical form";
    EXPECT_EQ(static_cast<int64_t>(entry.value().kind), row->getIf<int64_t>("kind").value_or(-1));
    if (entry.value().seq > 1) {
      EXPECT_TRUE(CheckPostChain(prev, prev_hash, entry.value()));
    }
    prev = entry.value();
    prev_hash = hash;
  }

  const Object* head = root->getObject("head");
  ASSERT_NE(head, nullptr);
  const ByteVector head_bytes = Base64Decode(head->getString("signed_b64").value_or("")).value();
  ASSERT_GT(head_bytes.size(), kMlDsa65SignatureBytes);
  const auto split = head_bytes.end() - static_cast<std::ptrdiff_t>(kMlDsa65SignatureBytes);
  const SignedPostHead signed_head{ByteVector(head_bytes.begin(), split), ByteVector(split, head_bytes.end())};
  auto parsed = VerifyPostHead(signed_head, key, kT0 + 3'600'000);
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_EQ(Hex(PostHeadHash(signed_head.canonical)), head->getString("head_hash_hex").value_or(""));
  ASSERT_EQ(parsed.value().lanes.size(), 1u);
  EXPECT_EQ(parsed.value().lanes[0].entry_hash, prev_hash) << "the head names the lane's last entry";

  const Object* media = root->getObject("media_presign");
  ASSERT_NE(media, nullptr);
  const ByteVector media_bytes = PostMediaPresignSignBytes(
      account, [&] {
        const ByteVector raw = HexToBytes(media->getString("content_id_hex").value_or("")).value();
        PostHash id{};
        std::copy(raw.begin(), raw.end(), id.begin());
        return id;
      }(),
      static_cast<uint64_t>(media->getIf<int64_t>("size").value_or(0)), media->getString("mime").value_or(""),
      media->getIf<int64_t>("issued_at").value_or(0));
  EXPECT_EQ(BytesToHex(media_bytes), media->getString("sign_bytes_hex").value_or(""));
  EXPECT_TRUE(MlDsa::Verify(key, media_bytes, Base64Decode(media->getString("signature_b64").value_or("")).value()).value());

  const Array* reports = root->getArray("reports");
  ASSERT_NE(reports, nullptr);
  ASSERT_EQ(reports->elements.size(), 2u);
  const ByteVector first_hash = HexToBytes(root->getString("first_entry_hash_hex").value_or("")).value();
  PostRef first{.author = account, .lane = {}, .seq = 1, .entry_hash = {}};
  const ByteVector lane = HexToBytes(root->getString("lane_hex").value_or("")).value();
  std::copy(lane.begin(), lane.end(), first.lane.begin());
  std::copy(first_hash.begin(), first_hash.end(), first.entry_hash.begin());
  const PostHash media_id = HashOfText("media");
  for (const Value& item : reports->elements) {
    const Object* report = asObject(item);
    ASSERT_NE(report, nullptr);
    const std::string reason = report->getString("reason").value_or("");
    const std::string note = report->getString("note").value_or("");
    const int64_t issued_at = report->getIf<int64_t>("issued_at").value_or(0);
    const ByteVector bytes = report->getString("subject").value_or("") == "entry"
                                 ? PostEntryReportSignBytes(account, first, reason, note, issued_at)
                                 : PostMediaReportSignBytes(account, media_id, reason, note, issued_at);
    EXPECT_EQ(BytesToHex(bytes), report->getString("sign_bytes_hex").value_or(""));
    EXPECT_TRUE(MlDsa::Verify(key, bytes, Base64Decode(report->getString("signature_b64").value_or("")).value()).value());
  }
}

} // namespace
} // namespace pbr
