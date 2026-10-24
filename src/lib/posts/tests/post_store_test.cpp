#include "lib/posts/PostAuthor.h"
#include "lib/posts/PostBodies.h"
#include "lib/posts/PostStore.h"
#include "lib/posts/tests/PostTestAccount.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

class PostStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_posts_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    store_ = std::make_unique<PostStore>((dir_ / "profile.db").string());
  }
  void TearDown() override {
    store_.reset();
    std::filesystem::remove_all(dir_);
  }

  PostAuthor AuthorOn(const std::string& device, PostLane lane) {
    return PostAuthor(*store_, alice_.account_id, device, alice_.Signer(), [lane]() { return lane; });
  }
  static ByteVector Text(const std::string& text) { return EncodePostBody({.text = text}); }

  std::filesystem::path dir_;
  std::unique_ptr<PostStore> store_;
  test::PostTestAccount alice_ = test::PostTestAccount::Make();
};

TEST_F(PostStoreTest, ADeviceWritesOneChainedLane) {
  PostAuthor author = AuthorOn("QmPhone", PostLane{1});
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(author.Append(PostKind::Post, Text("post " + std::to_string(i)), kNow - i * 1000))
        << "the clock went back; the lane's claimed time does not";
  }
  auto lanes = store_->LanesOf(alice_.account_id);
  ASSERT_TRUE(lanes);
  ASSERT_EQ(lanes.value().size(), 1u);
  EXPECT_EQ(lanes.value()[0].seq, 3u);

  auto range = store_->Range(alice_.account_id, PostLane{1}, 1, 10);
  ASSERT_TRUE(range);
  ASSERT_EQ(range.value().size(), 3u);
  PostEntry prev;
  PostHash prev_hash{};
  for (const StoredPostEntry& row : range.value()) {
    ASSERT_TRUE(row.signed_bytes.has_value());
    const SignedPostEntry signed_entry = DeserializeSignedPostEntry(*row.signed_bytes).value();
    auto entry = VerifyPostEntry(signed_entry, alice_.public_key, kNow);
    ASSERT_TRUE(entry) << entry.error().message;
    EXPECT_EQ(PostEntryHash(signed_entry.canonical), row.entry_hash);
    if (row.seq > 1) {
      EXPECT_TRUE(CheckPostChain(prev, prev_hash, entry.value()));
    }
    prev = entry.value();
    prev_hash = row.entry_hash;
  }
  EXPECT_EQ(store_->Range(alice_.account_id, PostLane{1}, 2, 10).value().size(), 2u);
}

TEST_F(PostStoreTest, EachDeviceHasItsLaneAndTheHeadNamesBoth) {
  PostAuthor phone = AuthorOn("QmPhone", PostLane{2});
  PostAuthor laptop = AuthorOn("QmLaptop", PostLane{1});
  ASSERT_TRUE(phone.Append(PostKind::Post, Text("from the phone"), kNow));
  ASSERT_TRUE(laptop.Append(PostKind::Post, Text("from the laptop"), kNow));
  ASSERT_TRUE(phone.Append(PostKind::Post, Text("phone again"), kNow + 1));

  auto head = phone.IssueHead(kNow + 2, {"https://relay.example/api/relay"});
  ASSERT_TRUE(head);
  auto parsed = VerifyPostHead(head.value(), alice_.public_key, kNow + 2);
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed.value().lanes.size(), 2u);
  EXPECT_EQ(parsed.value().lanes[0].lane, PostLane{1});
  EXPECT_EQ(parsed.value().lanes[0].seq, 1u);
  EXPECT_EQ(parsed.value().lanes[1].seq, 2u);
  EXPECT_EQ(parsed.value().targets, std::vector<std::string>{"https://relay.example/api/relay"});
  EXPECT_EQ(store_->Head(alice_.account_id).value()->canonical, head.value().canonical);
}

TEST_F(PostStoreTest, ADeviceNeverReusesASeqItLostTrackOf) {
  PostAuthor phone = AuthorOn("QmPhone", PostLane{1});
  ASSERT_TRUE(phone.Append(PostKind::Post, Text("one"), kNow));
  ASSERT_TRUE(phone.Append(PostKind::Post, Text("two"), kNow + 1));
  // The device's own record goes back (a restore): it believes seq 2 is next, but seq 2 is out there.
  OwnPostLane stale = store_->OwnLane("QmPhone").value().value();
  stale.next_seq = 2;
  ASSERT_TRUE(store_->PutOwnLane(stale));

  PostAuthor restored = AuthorOn("QmPhone", PostLane{7});
  ASSERT_TRUE(restored.Append(PostKind::Post, Text("after restore"), kNow + 2));
  EXPECT_FALSE(store_->HasFork(alice_.account_id).value()) << "no second entry at seq 2";
  auto lanes = store_->LanesOf(alice_.account_id).value();
  ASSERT_EQ(lanes.size(), 2u) << "it started a new lane";
  EXPECT_EQ(store_->OwnLane("QmPhone").value()->lane, PostLane{7});
}

TEST_F(PostStoreTest, ARivalEntryAtTheSameSeqIsAFork) {
  PostAuthor phone = AuthorOn("QmPhone", PostLane{1});
  ASSERT_TRUE(phone.Append(PostKind::Post, Text("one"), kNow));
  // The same account signs another seq-1 entry (a second device misusing the lane, or a lie).
  const PostEntry rival{.author = alice_.account_id, .lane = PostLane{1}, .seq = 1, .claimed_at = kNow,
                        .kind = static_cast<uint8_t>(PostKind::Post), .body = Text("one, rewritten")};
  const SignedPostEntry signed_rival = SignPostEntry(rival, alice_.Signer()).value();
  auto added = store_->AddEntry(signed_rival, rival, PostEntryHash(signed_rival.canonical));
  ASSERT_TRUE(added);
  EXPECT_EQ(added.value(), PostStore::Added::Fork);
  EXPECT_TRUE(store_->HasFork(alice_.account_id).value());
  EXPECT_EQ(store_->At(alice_.account_id, PostLane{1}, 1).value().size(), 2u) << "both kept, as proof";
  EXPECT_EQ(store_->AddEntry(signed_rival, rival, PostEntryHash(signed_rival.canonical)).value(),
            PostStore::Added::Duplicate);
}

TEST_F(PostStoreTest, StubsKeepThePlaceWithoutTheBytes) {
  PostAuthor phone = AuthorOn("QmPhone", PostLane{1});
  const SignedPostEntry first = phone.Append(PostKind::Post, Text("regret"), kNow).value();
  const PostEntry parsed = ParsePostEntry(first.canonical).value();
  const PostHash hash = PostEntryHash(first.canonical);

  ASSERT_TRUE(store_->StubOut(alice_.account_id, PostLane{1}, 1, hash, PostStub::Deleted));
  auto at = store_->At(alice_.account_id, PostLane{1}, 1).value();
  ASSERT_EQ(at.size(), 1u);
  EXPECT_FALSE(at[0].signed_bytes.has_value());
  EXPECT_EQ(at[0].stub, PostStub::Deleted);
  EXPECT_EQ(at[0].entry_hash, hash) << "the chain past it still verifies";
  ASSERT_TRUE(store_->AddEntry(first, parsed, hash));
  EXPECT_FALSE(store_->At(alice_.account_id, PostLane{1}, 1).value()[0].signed_bytes.has_value())
      << "a deleted entry stays deleted when a copy comes back";

  // An entry this reader would not keep (an unknown kind) stands as a stub, then fills when it is understood.
  const PostEntry later{.author = alice_.account_id, .lane = PostLane{5}, .seq = 1, .claimed_at = kNow,
                        .kind = static_cast<uint8_t>(PostKind::Post), .body = Text("later")};
  const SignedPostEntry signed_later = SignPostEntry(later, alice_.Signer()).value();
  const PostHash later_hash = PostEntryHash(signed_later.canonical);
  ASSERT_TRUE(store_->AddStub({.author = alice_.account_id, .lane = PostLane{5}, .seq = 1, .entry_hash = later_hash,
                               .kind = later.kind, .claimed_at = kNow, .size = 100, .stub = PostStub::Unkept}));
  EXPECT_FALSE(store_->At(alice_.account_id, PostLane{5}, 1).value()[0].signed_bytes.has_value());
  EXPECT_EQ(store_->AddEntry(signed_later, later, later_hash).value(), PostStore::Added::Duplicate);
  EXPECT_TRUE(store_->At(alice_.account_id, PostLane{5}, 1).value()[0].signed_bytes.has_value());
}

TEST_F(PostStoreTest, TheNewestHeadWins) {
  PostAuthor phone = AuthorOn("QmPhone", PostLane{1});
  ASSERT_TRUE(phone.Append(PostKind::Post, Text("one"), kNow));
  const SignedPostHead newer = phone.IssueHead(kNow + 10).value();
  const PostHead older{.author = alice_.account_id, .issued_at = kNow};
  const SignedPostHead signed_older = SignPostHead(older, alice_.Signer()).value();
  ASSERT_TRUE(store_->PutHead(signed_older, older));
  EXPECT_EQ(store_->Head(alice_.account_id).value()->canonical, newer.canonical);
}

TEST_F(PostStoreTest, TheWriterChecksTheBodyBeforeSigning) {
  PostAuthor phone = AuthorOn("QmPhone", PostLane{1});
  EXPECT_FALSE(phone.Append(PostKind::Post, Text(std::string(kMaxPostTextBytes + 1, 'a')), kNow));
  EXPECT_FALSE(store_->OwnLane("QmPhone").value().has_value()) << "nothing written, no lane taken";
}

} // namespace
} // namespace pbr
