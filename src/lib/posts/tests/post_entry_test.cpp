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
  const PostBody post{.text = "hi",
                      .media = {{.content_id = PostHash{1}, .mime = "image/png", .size = 42}},
                      .reply_to = ref,
                      .quote = std::nullopt};
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
