#include "lib/posts/PostBodies.h"
#include "lib/posts/PostEntry.h"
#include "lib/posts/tests/PostTestAccount.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

class PostEntryTest : public ::testing::Test {
protected:
  PostEntry First(const PostBody& body) const {
    return PostEntry{.author = alice_.account_id,
                     .lane = PostLane{1, 2, 3},
                     .seq = 1,
                     .claimed_at = kNow,
                     .kind = static_cast<uint8_t>(PostKind::Post),
                     .body = EncodePostBody(body)};
  }
  SignedPostEntry Sign(const PostEntry& entry) const { return SignPostEntry(entry, alice_.Signer()).value(); }
  Roe<PostEntry> Verify(const PostEntry& entry, int64_t now = kNow) const {
    return VerifyPostEntry(Sign(entry), alice_.public_key, now);
  }

  test::PostTestAccount alice_ = test::PostTestAccount::Make();
  test::PostTestAccount mallory_ = test::PostTestAccount::Make();
};

TEST_F(PostEntryTest, AnEntryIsItsBytesNotItsSignature) {
  const PostEntry entry = First({.text = "hello"});
  const SignedPostEntry once = Sign(entry);
  const SignedPostEntry twice = Sign(entry);
  EXPECT_EQ(once.canonical, twice.canonical);
  EXPECT_NE(once.signature, twice.signature) << "ML-DSA signatures are randomized";
  EXPECT_EQ(PostEntryHash(once.canonical), PostEntryHash(twice.canonical)) << "one entry, one identity (S004)";

  EXPECT_EQ(ParsePostEntry(once.canonical).value(), entry);
  auto back = DeserializeSignedPostEntry(SerializeSignedPostEntry(once));
  ASSERT_TRUE(back);
  EXPECT_EQ(back.value().canonical, once.canonical);
  EXPECT_EQ(back.value().signature, once.signature);
}

TEST_F(PostEntryTest, OnlyTheAuthorsKeyVouchesForIt) {
  const SignedPostEntry signed_entry = Sign(First({.text = "hello"}));
  EXPECT_TRUE(VerifyPostEntry(signed_entry, alice_.public_key, kNow));
  EXPECT_FALSE(VerifyPostEntry(signed_entry, mallory_.public_key, kNow)) << "a key that is not the author's";

  SignedPostEntry tampered = signed_entry;
  tampered.canonical.back() ^= 1;
  EXPECT_FALSE(VerifyPostEntry(tampered, alice_.public_key, kNow));

  // Mallory signs an entry naming Alice as its author.
  const SignedPostEntry forged = SignPostEntry(First({.text = "hello"}), mallory_.Signer()).value();
  EXPECT_FALSE(VerifyPostEntry(forged, alice_.public_key, kNow));
  EXPECT_FALSE(VerifyPostEntry(forged, mallory_.public_key, kNow));
}

TEST_F(PostEntryTest, SeqAndPrevAgree) {
  PostEntry entry = First({.text = "hello"});
  entry.prev[0] = 1;
  EXPECT_FALSE(Verify(entry)) << "the first entry has no predecessor";
  entry.seq = 2;
  EXPECT_TRUE(Verify(entry));
  entry.prev = PostHash{};
  EXPECT_FALSE(Verify(entry)) << "a later entry names its predecessor";
  entry.seq = 0;
  EXPECT_FALSE(Verify(entry));
}

TEST_F(PostEntryTest, AClaimedTimeMayNotBeInTheFuture) {
  PostEntry entry = First({.text = "hello"});
  entry.claimed_at = kNow + 4 * 60 * 1000;
  EXPECT_TRUE(Verify(entry)) << "clock skew allowed";
  entry.claimed_at = kNow + 6 * 60 * 1000;
  EXPECT_FALSE(Verify(entry));
}

TEST_F(PostEntryTest, TheProtocolLimitsHold) {
  EXPECT_TRUE(Verify(First({.text = std::string(kMaxPostTextBytes, 'a')})));
  EXPECT_FALSE(Verify(First({.text = std::string(kMaxPostTextBytes + 1, 'a')})));
  EXPECT_FALSE(Verify(First({.text = "\xC3\x28"}))) << "not UTF-8";
  EXPECT_FALSE(Verify(First({}))) << "nothing to post";

  PostBody media_post{.text = "pics"};
  for (size_t i = 0; i < kMaxPostMedia; ++i) {
    media_post.media.push_back({.content_id = PostHash{static_cast<uint8_t>(i + 1)}, .mime = "image/jpeg", .size = 1});
  }
  EXPECT_TRUE(Verify(First(media_post)));
  media_post.media.push_back({.content_id = PostHash{9}, .mime = "image/jpeg", .size = 1});
  EXPECT_FALSE(Verify(First(media_post))) << "too many media";
  media_post.media.pop_back();
  media_post.media[0].size = kMaxPostMediaObjectBytes + 1;
  EXPECT_FALSE(Verify(First(media_post))) << "a media object too large";

  // The whole entry: a profile whose services push the canonical bytes past 8 KiB.
  ProfileBody profile;
  for (size_t i = 0; i < kMaxPostServices; ++i) {
    profile.services.push_back({.kind = "post_relay", .terms = std::string(kMaxPostServiceTermsBytes, 'x')});
  }
  PostEntry big = First({.text = "x"});
  big.kind = static_cast<uint8_t>(PostKind::Profile);
  big.body = EncodeProfileBody(profile);
  EXPECT_FALSE(SignPostEntry(big, alice_.Signer())) << "the author cannot sign past the entry limit";
  EXPECT_FALSE(ParsePostEntry(PostEntryCanonicalBytes(big))) << "and no reader parses it";
}

TEST_F(PostEntryTest, EditsDeletesAndDisavowalsAreOfOnesOwnEntries) {
  const PostRef own{.author = alice_.account_id, .lane = PostLane{1}, .seq = 1, .entry_hash = PostHash{7}};
  PostRef others = own;
  others.author = mallory_.account_id;

  PostEntry entry = First({.text = "x"});
  entry.seq = 2;
  entry.prev = PostHash{1};
  auto with = [&](PostKind kind, ByteVector body) {
    entry.kind = static_cast<uint8_t>(kind);
    entry.body = std::move(body);
    return static_cast<bool>(Verify(entry));
  };
  EXPECT_TRUE(with(PostKind::Edit, EncodeEditBody({.target = own, .replacement = {.text = "fixed"}})));
  EXPECT_FALSE(with(PostKind::Edit, EncodeEditBody({.target = others, .replacement = {.text = "fixed"}})));
  EXPECT_TRUE(with(PostKind::Delete, EncodeTargetBody({.target = own})));
  EXPECT_FALSE(with(PostKind::Delete, EncodeTargetBody({.target = others})));
  EXPECT_FALSE(with(PostKind::Disavow, EncodeDisavowBody({.targets = {own, others}})));
  EXPECT_TRUE(with(PostKind::React, EncodeReactBody({.target = others, .reaction = "like"})))
      << "a like is about anyone's post";
  EXPECT_TRUE(with(PostKind::Repost, EncodeTargetBody({.target = others})));
}

TEST_F(PostEntryTest, ThePostBodyCarriesWhatReadersNeedWithinLimits) {
  // S026: what a reader needs before the media arrives and for its filters, each field bounded.
  const PostRef ref{.author = mallory_.account_id, .lane = PostLane{4}, .seq = 3, .entry_hash = PostHash{5}};
  const PostBody full{.text = "hi @mallory see https://x.example #\xE4\xB8\xAD\xE6\x96\x87",
                      .media = {{.content_id = PostHash{1}, .mime = "video/mp4", .size = 10, .alt = "a clip",
                                 .width = 1920, .height = 1080, .duration_ms = 4000,
                                 .placeholder = ByteVector(kMaxPostPlaceholderBytes, 7)}},
                      .facets = {{.start = 3, .end = 11, .type = 1, .value = mallory_.account_id},
                                 {.start = 16, .end = 33, .type = 2, .value = "https://x.example"},
                                 {.start = 34, .end = 41, .type = 3, .value = "\xE4\xB8\xAD\xE6\x96\x87"}},
                      .reply = PostReply{.root = ref, .parent = ref},
                      .content_warning = std::string(kMaxPostContentWarningBytes, 'w'),
                      .labels = {"sexual", "nudity", "graphic", "made-up-1"},
                      .languages = {"en", "zh-Hans", "fr"}};
  auto ok = Verify(First(full));
  ASSERT_TRUE(ok) << ok.error().message;

  auto fails = [&](auto change, const char* why) {
    PostBody body = full;
    change(body);
    EXPECT_FALSE(Verify(First(body))) << why;
  };
  fails([](PostBody& b) { b.media[0].alt = std::string(kMaxPostAltBytes + 1, 'a'); }, "alt text too long");
  fails([](PostBody& b) { b.media[0].placeholder.push_back(0); }, "placeholder too long");
  fails([](PostBody& b) { b.facets[1].start = 10; }, "facets overlap");
  fails([](PostBody& b) { std::swap(b.facets[0], b.facets[1]); }, "facets out of order");
  fails([](PostBody& b) { b.facets[2].end = 40; }, "a facet ends inside a UTF-8 sequence");
  fails([](PostBody& b) { b.facets[2].end = b.text.size() + 1; }, "a facet past the text");
  fails([](PostBody& b) { b.facets[0].end = b.facets[0].start; }, "an empty facet");
  fails([](PostBody& b) { b.facets[1].value = "http://x.example"; }, "links are https");
  fails([](PostBody& b) { b.facets[2].value = "#tag"; }, "a hashtag without its #");
  fails([](PostBody& b) { b.facets[0].value.clear(); }, "a mention names an account");
  fails([](PostBody& b) { b.content_warning.push_back('w'); }, "content warning too long");
  fails([](PostBody& b) { b.labels.push_back("spoiler"); }, "too many labels");
  fails([](PostBody& b) { b.labels[0] = "Sexual"; }, "labels are lowercase tokens");
  fails([](PostBody& b) { b.languages.push_back("de"); }, "too many languages");
  fails([](PostBody& b) { b.languages[0] = "en US"; }, "a language is a tag");
  fails([](PostBody& b) { b.reply->root.seq = 0; }, "a malformed thread root");

  PostBody unknown_facet = full;
  unknown_facet.facets[1].type = 42;
  EXPECT_TRUE(Verify(First(unknown_facet))) << "an unknown facet type is shown as plain text";

  PostBody many_mentions{.text = std::string(kMaxPostMentions + 1, 'm')};
  for (uint64_t i = 0; i <= kMaxPostMentions; ++i) {
    many_mentions.facets.push_back({.start = i, .end = i + 1, .type = 1, .value = mallory_.account_id});
  }
  EXPECT_FALSE(Verify(First(many_mentions))) << "too many mentions";
  many_mentions.facets.pop_back();
  EXPECT_TRUE(Verify(First(many_mentions)));
}

TEST_F(PostEntryTest, ExtensionsAreSignedSkippableAndInOrder) {
  // S027: additive fields land at the end of any entry, inside what is hashed and signed.
  PostEntry plain = First({.text = "hello"});
  PostEntry extended = plain;
  extended.extensions = {{.tag = 7, .value = ByteVector{1}}, {.tag = 300, .value = {}}};
  const SignedPostEntry signed_entry = Sign(extended);
  auto verified = VerifyPostEntry(signed_entry, alice_.public_key, kNow);
  ASSERT_TRUE(verified) << verified.error().message;
  EXPECT_EQ(verified.value(), extended) << "tags this version does not know are kept, not refused";
  EXPECT_NE(PostEntryHash(signed_entry.canonical), PostEntryHash(PostEntryCanonicalBytes(plain)))
      << "an extension is part of the entry's identity";

  PostEntry unordered = plain;
  unordered.extensions = {{.tag = 9}, {.tag = 9}};
  EXPECT_FALSE(SignPostEntry(unordered, alice_.Signer())) << "a tag at most once";
  EXPECT_FALSE(ParsePostEntry(PostEntryCanonicalBytes(unordered))) << "and no reader takes a second form";
  unordered.extensions = {{.tag = 9}, {.tag = 8}};
  EXPECT_FALSE(ParsePostEntry(PostEntryCanonicalBytes(unordered)));

  PostEntry crowded = plain;
  for (uint16_t tag = 1; tag <= kMaxPostExtensions + 1; ++tag) {
    crowded.extensions.push_back({.tag = tag});
  }
  EXPECT_FALSE(SignPostEntry(crowded, alice_.Signer()));
  EXPECT_FALSE(ParsePostEntry(PostEntryCanonicalBytes(crowded)));
}

TEST_F(PostEntryTest, AnUnknownKindIsStillAValidEntry) {
  PostEntry entry = First({.text = "x"});
  entry.kind = 99;
  entry.body = ByteVector{1, 2, 3};
  EXPECT_TRUE(Verify(entry)) << "a newer client's kind chains like any other (S016)";
  EXPECT_FALSE(IsKnownPostKind(99));
}

TEST_F(PostEntryTest, ALaneChainsAndAForkIsCaught) {
  const SignedPostEntry first = Sign(First({.text = "one"}));
  const PostEntry first_entry = ParsePostEntry(first.canonical).value();
  const PostHash first_hash = PostEntryHash(first.canonical);

  PostEntry second = First({.text = "two"});
  second.seq = 2;
  second.prev = first_hash;
  second.claimed_at = kNow + 1;
  EXPECT_TRUE(CheckPostChain(first_entry, first_hash, second));

  PostEntry wrong_prev = second;
  wrong_prev.prev[0] ^= 1;
  EXPECT_FALSE(CheckPostChain(first_entry, first_hash, wrong_prev));
  PostEntry back_in_time = second;
  back_in_time.claimed_at = kNow - 1;
  EXPECT_FALSE(CheckPostChain(first_entry, first_hash, back_in_time));

  PostEntry rival = second;
  rival.body = EncodePostBody({.text = "two, rewritten"});
  const PostHash second_hash = PostEntryHash(PostEntryCanonicalBytes(second));
  const PostHash rival_hash = PostEntryHash(PostEntryCanonicalBytes(rival));
  EXPECT_TRUE(IsPostFork(second, second_hash, rival, rival_hash));
  EXPECT_FALSE(IsPostFork(second, second_hash, second, second_hash));
}

TEST(PostBodiesTest, EveryKindRoundTrips) {
  const PostRef ref{.author = "account:a", .lane = PostLane{4}, .seq = 3, .entry_hash = PostHash{5}};
  const PostBody post{.text = "hi @a #tag",
                      .media = {{.content_id = PostHash{1}, .mime = "image/png", .size = 42, .alt = "a cat",
                                 .width = 640, .height = 480, .placeholder = ByteVector{1, 2, 3}}},
                      .facets = {{.start = 3, .end = 5, .type = 1, .value = "account:a"},
                                 {.start = 6, .end = 10, .type = 3, .value = "tag"}},
                      .reply = PostReply{.root = ref, .parent = ref},
                      .quote = ref,
                      .content_warning = "spoilers",
                      .labels = {"graphic"},
                      .languages = {"en", "zh-Hans"}};
  EXPECT_EQ(DecodePostBody(EncodePostBody(post)).value(), post);
  const EditBody edit{.target = ref, .replacement = post};
  EXPECT_EQ(DecodeEditBody(EncodeEditBody(edit)).value(), edit);
  const ReactBody react{.target = ref, .reaction = "like"};
  EXPECT_EQ(DecodeReactBody(EncodeReactBody(react)).value(), react);
  const ProfileBody profile{.display_name = "Alice", .bio = "hi", .avatar = PostHash{2},
                            .services = {{.kind = "witness", .terms = "{}"}}};
  EXPECT_EQ(DecodeProfileBody(EncodeProfileBody(profile)).value(), profile);
  const LaneCloseBody close{.lane = PostLane{6}, .last_seq = 9};
  EXPECT_EQ(DecodeLaneCloseBody(EncodeLaneCloseBody(close)).value(), close);
  const DisavowBody disavow{.targets = {ref, ref}};
  EXPECT_EQ(DecodeDisavowBody(EncodeDisavowBody(disavow)).value(), disavow);

  ByteVector trailing = EncodeReactBody(react);
  trailing.push_back(0);
  EXPECT_FALSE(DecodeReactBody(trailing)) << "nothing may follow a body";
}

TEST(PostBodiesTest, Utf8MeansWellFormed) {
  EXPECT_TRUE(IsWellFormedUtf8("plain"));
  EXPECT_TRUE(IsWellFormedUtf8("\xE4\xBD\xA0\xE5\xA5\xBD"));        // 你好
  EXPECT_TRUE(IsWellFormedUtf8("\xF0\x9F\x91\x8D"));                // 👍
  EXPECT_FALSE(IsWellFormedUtf8("\xC0\xAF"));                       // overlong
  EXPECT_FALSE(IsWellFormedUtf8("\xED\xA0\x80"));                   // surrogate
  EXPECT_FALSE(IsWellFormedUtf8("\xF4\x90\x80\x80"));               // past U+10FFFF
  EXPECT_FALSE(IsWellFormedUtf8("\xE4\xBD"));                       // cut short
}

TEST_F(PostEntryTest, AHeadNamesEveryLaneOnceInOrder) {
  PostHead head{.author = alice_.account_id,
                .issued_at = kNow,
                .lanes = {{.lane = PostLane{9}, .seq = 2, .entry_hash = PostHash{1}},
                          {.lane = PostLane{3}, .seq = 5, .entry_hash = PostHash{2}}}};
  const SignedPostHead signed_head = SignPostHead(head, alice_.Signer()).value();
  auto verified = VerifyPostHead(signed_head, alice_.public_key, kNow);
  ASSERT_TRUE(verified) << verified.error().message;
  ASSERT_EQ(verified.value().lanes.size(), 2u);
  EXPECT_EQ(verified.value().lanes[0].lane, PostLane{3}) << "lanes are sorted when encoded";
  EXPECT_FALSE(VerifyPostHead(signed_head, mallory_.public_key, kNow));
  EXPECT_FALSE(VerifyPostHead(SignPostHead({.author = alice_.account_id, .issued_at = kNow + 3'600'000}, alice_.Signer()).value(),
                              alice_.public_key, kNow))
      << "a head from the future";

  head.lanes.clear();
  for (size_t i = 0; i <= kMaxPostHeadLanes; ++i) {
    head.lanes.push_back({.lane = PostLane{static_cast<uint8_t>(i + 1)}, .seq = 1});
  }
  EXPECT_FALSE(SignPostHead(head, alice_.Signer())) << "more lanes than a head may name";
  EXPECT_FALSE(ParsePostHead(PostHeadCanonicalBytes(head)));

  const PostHead plain_http{.author = alice_.account_id, .issued_at = kNow, .targets = {"http://relay.example"}};
  EXPECT_FALSE(SignPostHead(plain_http, alice_.Signer())) << "targets are https";
  EXPECT_FALSE(ParsePostHead(PostHeadCanonicalBytes(plain_http)));

  PostHead repeated{.author = alice_.account_id, .issued_at = kNow,
                    .lanes = {{.lane = PostLane{1}, .seq = 1}, {.lane = PostLane{1}, .seq = 2}}};
  EXPECT_FALSE(ParsePostHead(PostHeadCanonicalBytes(repeated))) << "a lane named twice";
}

} // namespace
} // namespace pbr
