#include "feature/settings/StorageManagerModel.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

std::vector<uint8_t> Hash(const int n) {
  return std::vector<uint8_t>(32, static_cast<uint8_t>(n));
}

pbr::StoredAttachmentItem Item(const std::string& thread, const int hash, const std::string& mime, const uint64_t bytes,
                               const int64_t ts = 0, const bool server = true) {
  pbr::StoredAttachmentItem item;
  item.thread_id = thread;
  item.message_id = thread + "-m" + std::to_string(hash) + "-" + std::to_string(ts);
  item.content_hash = Hash(hash);
  item.mime = mime;
  item.filename = "f" + std::to_string(hash);
  item.bytes = bytes;
  item.timestamp_ms = ts;
  item.has_server_copy = server;
  return item;
}

} // namespace

TEST(StorageManagerModelTest, KindsByMime) {
  EXPECT_EQ(pbr::StoredKindOfMime("image/jpeg"), pbr::StoredKind::Picture);
  EXPECT_EQ(pbr::StoredKindOfMime("image/heic"), pbr::StoredKind::Picture);
  EXPECT_EQ(pbr::StoredKindOfMime("video/quicktime"), pbr::StoredKind::Video);
  EXPECT_EQ(pbr::StoredKindOfMime("application/pdf"), pbr::StoredKind::File);
  EXPECT_EQ(pbr::StoredKindOfMime(""), pbr::StoredKind::File);
  EXPECT_EQ(pbr::StoredKindFromId("video"), pbr::StoredKind::Video);
  EXPECT_FALSE(pbr::StoredKindFromId("other").has_value());
}

// Grouped by chat, most bytes first; within a chat and across chats a hash counts once; only chats holding
// something are listed; the sizes are the items' stored bytes.
TEST(StorageManagerModelTest, GroupsByChatSortsBySizeAndCountsEachHashOnce) {
  const std::vector<pbr::StoredAttachmentItem> items = {
      Item("a", 1, "image/jpeg", 2 * kMiB),
      Item("a", 2, "video/mp4", 10 * kMiB),
      Item("a", 3, "application/pdf", 1 * kMiB),
      Item("b", 1, "image/jpeg", 2 * kMiB), // the same picture forwarded into b
      Item("b", 4, "image/png", 30 * kMiB),
      Item("c", 5, "application/zip", 512),
  };
  const pbr::StorageOverview overview = pbr::BuildStorageOverview(items);

  ASSERT_EQ(overview.chats.size(), 3u);
  EXPECT_EQ(overview.chats[0].thread_id, "b");
  EXPECT_EQ(overview.chats[0].totals.Bytes(), 32 * kMiB);
  EXPECT_EQ(overview.chats[0].totals.pictures, 2u);
  EXPECT_EQ(overview.chats[1].thread_id, "a");
  EXPECT_EQ(overview.chats[1].totals.Bytes(), 13 * kMiB);
  EXPECT_EQ(overview.chats[1].totals.pictures, 1u);
  EXPECT_EQ(overview.chats[1].totals.videos, 1u);
  EXPECT_EQ(overview.chats[1].totals.files, 1u);
  EXPECT_EQ(overview.chats[2].thread_id, "c");

  // Globally the forwarded picture counts once.
  EXPECT_EQ(overview.totals.pictures, 2u);
  EXPECT_EQ(overview.totals.picture_bytes, 32 * kMiB);
  EXPECT_EQ(overview.totals.videos, 1u);
  EXPECT_EQ(overview.totals.video_bytes, 10 * kMiB);
  EXPECT_EQ(overview.totals.files, 2u);
  EXPECT_EQ(overview.totals.file_bytes, 1 * kMiB + 512);
  EXPECT_EQ(overview.totals.Bytes(), 43 * kMiB + 512);

  // A chat holding nothing on this device is not in the list at all.
  EXPECT_TRUE(pbr::BuildStorageOverview({}).chats.empty());
}

// "Files larger than 5 MB" goes by the stored bytes, crosses chats, lists each hash once (its newest message).
TEST(StorageManagerModelTest, LargeListCrossesChatsOncePerHash) {
  const std::vector<pbr::StoredAttachmentItem> items = {
      Item("a", 1, "video/mp4", 40 * kMiB, /*ts=*/100),
      Item("b", 1, "video/mp4", 40 * kMiB, /*ts=*/200),
      Item("a", 2, "application/zip", 6 * kMiB),
      Item("b", 3, "image/jpeg", 5 * kMiB), // exactly 5 MB is not "larger than"
      Item("c", 4, "image/jpeg", 2 * kMiB),
  };
  const pbr::StorageOverview overview = pbr::BuildStorageOverview(items);
  ASSERT_EQ(overview.large.size(), 2u);
  EXPECT_EQ(overview.large[0].content_hash, Hash(1));
  EXPECT_EQ(overview.large[0].thread_id, "b") << "the newest message represents the hash";
  EXPECT_EQ(overview.large[1].content_hash, Hash(2));
  EXPECT_EQ(overview.large_bytes, 46 * kMiB);

  EXPECT_EQ(pbr::StorageItemsOfKind(overview.large, pbr::StoredKind::Video).size(), 1u);
  EXPECT_EQ(pbr::StorageItemsOfKind(overview.large, pbr::StoredKind::File).size(), 1u);
  EXPECT_TRUE(pbr::StorageItemsOfKind(overview.large, pbr::StoredKind::Picture).empty());

  // Deleting the shared video from this list names it in every chat that holds it.
  const auto refs = pbr::StorageDeleteRefs(items, {overview.large[0]}, /*every_chat=*/true);
  ASSERT_EQ(refs.size(), 2u);
  EXPECT_EQ(refs[0].first, "a");
  EXPECT_EQ(refs[1].first, "b");
  // From a chat's page only that chat's pair.
  const auto own = pbr::StorageDeleteRefs(items, {items[0]}, /*every_chat=*/false);
  ASSERT_EQ(own.size(), 1u);
  EXPECT_EQ(own[0].first, "a");
}

TEST(StorageManagerModelTest, ChatItemsByKindNewestFirst) {
  const std::vector<pbr::StoredAttachmentItem> items = {
      Item("a", 1, "image/jpeg", 10, 100),
      Item("a", 2, "image/png", 10, 300),
      Item("a", 3, "video/mp4", 10, 200),
      Item("b", 4, "image/jpeg", 10, 400),
  };
  const auto pictures = pbr::StorageItemsOfChat(items, "a", pbr::StoredKind::Picture);
  ASSERT_EQ(pictures.size(), 2u);
  EXPECT_EQ(pictures[0].content_hash, Hash(2));
  EXPECT_EQ(pictures[1].content_hash, Hash(1));
  EXPECT_EQ(pbr::StorageItemsOfChat(items, "a", pbr::StoredKind::Video).size(), 1u);
  EXPECT_TRUE(pbr::StorageItemsOfChat(items, "a", pbr::StoredKind::File).empty());
}

// What the confirmation adds: items only on this device, items another chat still holds (not freed).
TEST(StorageManagerModelTest, DeleteFactsCountOnlyHereAndSharedElsewhere) {
  const std::vector<pbr::StoredAttachmentItem> items = {
      Item("a", 1, "image/jpeg", 3 * kMiB, 0, /*server=*/true),
      Item("b", 1, "image/jpeg", 3 * kMiB, 0, /*server=*/true),  // also in b
      Item("a", 2, "image/jpeg", 4 * kMiB, 0, /*server=*/false), // never uploaded
      Item("a", 3, "application/pdf", 1 * kMiB, 0, /*server=*/true),
  };
  const std::vector<pbr::StoredAttachmentItem> selected = {items[0], items[2], items[3]};
  const auto refs = pbr::StorageDeleteRefs(items, selected, /*every_chat=*/false);
  const pbr::StorageDeleteFacts facts = pbr::StorageDeleteFactsOf(items, selected, refs);
  EXPECT_EQ(facts.count, 3u);
  EXPECT_EQ(facts.only_here, 1u);
  EXPECT_EQ(facts.shared_elsewhere, 1u) << "b still holds hash 1: its space is not freed";
  EXPECT_EQ(pbr::StorageSelectedBytes(selected), 8 * kMiB);

  // From the large list every chat is named, so nothing is kept elsewhere.
  const auto all_refs = pbr::StorageDeleteRefs(items, {items[0]}, /*every_chat=*/true);
  EXPECT_EQ(pbr::StorageDeleteFactsOf(items, {items[0]}, all_refs).shared_elsewhere, 0u);
}

// Chat history is the message databases, never more than what the attachments leave; the rest is "other".
TEST(StorageManagerModelTest, ReleasableBytesFromTheListCountAHashOnlyWhenNoOtherChatHoldsIt) {
  const std::vector<pbr::StoredAttachmentItem> all = {
      Item("a", 1, "image/jpeg", 3 * kMiB), Item("b", 1, "image/jpeg", 3 * kMiB), // shared by a and b
      Item("a", 2, "image/jpeg", 4 * kMiB),                                       // only in a
      Item("ai", 3, "image/png", 2 * kMiB), Item("a", 3, "image/png", 2 * kMiB),  // also an AI image
  };
  const pbr::StorageHashUses uses = pbr::BuildStorageHashUses(all);
  EXPECT_EQ(pbr::StorageReleasableBytes(uses, {}), 0u);
  EXPECT_EQ(pbr::StorageReleasableBytes(uses, {{"a", Hash(1)}}), 0u) << "b still holds it";
  EXPECT_EQ(pbr::StorageReleasableBytes(uses, {{"a", Hash(2)}}), 4 * kMiB);
  EXPECT_EQ(pbr::StorageReleasableBytes(uses, {{"a", Hash(1)}, {"a", Hash(2)}, {"a", Hash(3)}}), 4 * kMiB);
  EXPECT_EQ(pbr::StorageReleasableBytes(uses, {{"a", Hash(1)}, {"b", Hash(1)}}), 3 * kMiB) << "every chat: once";
  EXPECT_EQ(pbr::StorageReleasableBytes(uses, {{"a", Hash(9)}}), 0u) << "not in the list";
}

TEST(StorageManagerModelTest, UsageSplitsTheProfileTotal) {
  pbr::StorageKindTotals attachments;
  attachments.picture_bytes = 400;
  attachments.video_bytes = 300;
  attachments.file_bytes = 100;

  pbr::StorageUsage usage = pbr::ComputeStorageUsage(1000, attachments, 150);
  EXPECT_EQ(usage.total, 1000u);
  EXPECT_EQ(usage.pictures, 400u);
  EXPECT_EQ(usage.chats, 150u);
  EXPECT_EQ(usage.other, 50u);

  // The databases measured bigger than what is left: chat history = total − attachments, no "other".
  usage = pbr::ComputeStorageUsage(1000, attachments, 500);
  EXPECT_EQ(usage.chats, 200u);
  EXPECT_EQ(usage.other, 0u);

  // The folder count ran before a download finished: the total grows to fit.
  usage = pbr::ComputeStorageUsage(500, attachments, 100);
  EXPECT_EQ(usage.total, 800u);
  EXPECT_EQ(usage.chats, 0u);

  const auto segments = pbr::StorageBarSegments(pbr::ComputeStorageUsage(1000, attachments, 150), true);
  ASSERT_EQ(segments.size(), 5u);
  EXPECT_EQ(segments[0].kind, "picture");
  EXPECT_EQ(segments[3].kind, "chats");
  EXPECT_EQ(segments[4].kind, "other");
  EXPECT_NEAR(segments[0].percent, 40.f, 0.01f);
  EXPECT_NEAR(segments[4].percent, 5.f, 0.01f);

  // The manage page's bar: the three media kinds only, empty kinds left out.
  attachments.video_bytes = 0;
  const auto media = pbr::StorageBarSegments(pbr::ComputeStorageUsage(1000, attachments, 150), false);
  ASSERT_EQ(media.size(), 2u);
  EXPECT_EQ(media[1].kind, "file");
  EXPECT_NEAR(media[0].percent + media[1].percent, 100.f, 0.01f);
}

TEST(StorageManagerModelTest, FormatsSizes) {
  EXPECT_EQ(pbr::FormatByteSize(512), "512 B");
  EXPECT_EQ(pbr::FormatByteSize(8 * 1024 + 400), "8.4 KB");
  EXPECT_EQ(pbr::FormatByteSize(42 * kMiB), "42 MB");
  EXPECT_EQ(pbr::FormatByteSize(1288490189ull), "1.2 GB");
}

// Twenty changes in a burst after a count: at most one more count, and not before the interval.
TEST(StorageManagerModelTest, RecomputeGateCoalescesBursts) {
  using Step = pbr::StorageRecomputeGate::Step;
  pbr::StorageRecomputeGate gate(2000);
  int runs = 0;
  int64_t now = 0;
  gate.OnStarted(now); // the count on entering
  ++runs;
  EXPECT_EQ(gate.OnFinished(100).step, Step::Nothing);

  int64_t timer_at = -1;
  for (int i = 0; i < 20; ++i) {
    now = 200 + i * 50;
    const auto decision = gate.OnChange(now);
    if (decision.step == Step::RunNow) {
      gate.OnStarted(now);
      ++runs;
    } else if (decision.step == Step::RunLater) {
      EXPECT_EQ(timer_at, -1) << "only one pending";
      timer_at = now + decision.delay_ms;
    }
  }
  EXPECT_EQ(runs, 1);
  ASSERT_EQ(timer_at, 2000);
  const auto fire = gate.OnTimer(timer_at);
  ASSERT_EQ(fire.step, Step::RunNow);
  gate.OnStarted(timer_at);
  ++runs;
  // A change while that count runs only marks stale; the finish asks for the next one, an interval later.
  EXPECT_EQ(gate.OnChange(2100).step, Step::Nothing);
  const auto after = gate.OnFinished(2300);
  EXPECT_EQ(after.step, Step::RunLater);
  EXPECT_EQ(after.delay_ms, 1700);
  EXPECT_EQ(runs, 2);

  // The page closed: a timer still pending does nothing.
  gate.Reset();
  EXPECT_EQ(gate.OnTimer(4000).step, Step::Nothing);
}

// 3000 attachments in 50 chats: grouping, totals and the large list well under 200 ms (on a worker anyway).
TEST(StorageManagerModelTest, ThreeThousandItemsAreQuick) {
  std::vector<pbr::StoredAttachmentItem> items;
  items.reserve(3000);
  for (int i = 0; i < 3000; ++i) {
    pbr::StoredAttachmentItem item = Item("chat" + std::to_string(i % 50), 0, i % 3 == 0 ? "video/mp4" : "image/jpeg",
                                          static_cast<uint64_t>((i % 17) + 1) * kMiB / 2, i);
    item.content_hash = std::vector<uint8_t>(32, 0);
    item.content_hash[0] = static_cast<uint8_t>(i & 0xff);
    item.content_hash[1] = static_cast<uint8_t>((i >> 8) & 0xff);
    items.push_back(std::move(item));
  }
  const auto started = std::chrono::steady_clock::now();
  const pbr::StorageOverview overview = pbr::BuildStorageOverview(items);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  EXPECT_EQ(overview.chats.size(), 50u);
  EXPECT_EQ(overview.totals.Count(), 3000u);
  EXPECT_LT(elapsed, 200);
}
