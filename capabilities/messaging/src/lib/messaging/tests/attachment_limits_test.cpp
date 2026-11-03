#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentLimits.h"
#include "lib/messaging/ChatAttachmentPrepare.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AttachmentLimitsTest, VideoSecondsDefaultToSixtyAndVideoWithinLimitIsInclusive) {
  AttachmentLimits limits = DefaultAttachmentLimits();
  EXPECT_EQ(limits.video_max_seconds, 60);
  EXPECT_TRUE(VideoWithinLimit(0, limits));
  EXPECT_TRUE(VideoWithinLimit(limits.max_video_bytes, limits));
  EXPECT_FALSE(VideoWithinLimit(limits.max_video_bytes + 1, limits));
  limits.max_video_bytes = 100'000'000;
  EXPECT_TRUE(VideoWithinLimit(100'000'000 - 1, limits)) << "follows the video limit, not the default";
}

TEST(AttachmentLimitsTest, ShippingDefaultsAreFiftyMegabytesForSendAndReceive) {
  const AttachmentLimits limits = DefaultAttachmentLimits();
  // One 50 MB limit for send and receive; shrunk pictures and automatic downloads stay at 4 MB.
  EXPECT_EQ(kLargeAttachmentMaxBytes, 50'000'000u);
  EXPECT_EQ(kDefaultAttachmentSendMaxBytes, kLargeAttachmentMaxBytes);
  EXPECT_EQ(kDefaultAttachmentReceiveMaxBytes, kLargeAttachmentMaxBytes);
  EXPECT_EQ(limits.max_original_image_bytes, kDefaultAttachmentSendMaxBytes);
  EXPECT_EQ(limits.max_file_bytes, kDefaultAttachmentSendMaxBytes);
  EXPECT_EQ(limits.max_video_bytes, kDefaultAttachmentSendMaxBytes);
  EXPECT_EQ(limits.max_audio_bytes, kDefaultAttachmentSendMaxBytes);
  EXPECT_EQ(limits.max_receive_bytes, kDefaultAttachmentReceiveMaxBytes);
  EXPECT_EQ(limits.max_image_bytes, 4'000'000u);
  EXPECT_EQ(limits.auto_download_max_bytes, 4'000'000u);
  EXPECT_EQ(limits.stay_hint_bytes, 10'000'000u);
  EXPECT_EQ(limits.voice_max_seconds, 120);
  EXPECT_EQ(limits.warn_percent_low, 80);
  EXPECT_EQ(limits.warn_percent_high, 90);
  // 2b
  EXPECT_EQ(limits.send_max_per_minute, 10);
  EXPECT_EQ(limits.auto_download_max_count_per_minute, 10);
  EXPECT_EQ(limits.auto_download_max_bytes_per_minute, 20'000'000u);
  EXPECT_EQ(limits.auto_download_all_max_bytes_per_minute, 80'000'000u);
  EXPECT_EQ(limits.auto_download_disk_cap_bytes, 1'000'000'000u);
  EXPECT_EQ(limits.auto_download_small_disk_cap_bytes, 1'500'000'000u);
}

TEST(AutoDownloadBudgetTest, ARefundGivesBackOneDownloadOfThatSize) {
  AutoDownloadBudget budget;
  EXPECT_TRUE(budget.TryAcquire("alice", 50'000'000, 1'000, 10, 80'000'000));
  EXPECT_FALSE(budget.TryAcquire("alice", 50'000'000, 1'000, 10, 80'000'000)) << "100 MB > 80 MB in the minute";
  budget.Refund("alice", 50'000'000);
  EXPECT_TRUE(budget.TryAcquire("alice", 50'000'000, 1'000, 10, 80'000'000)) << "the first one gave its bytes back";
  budget.Refund("bob", 1);  // nothing recorded: no effect
  budget.Refund("alice", 7);  // no entry of that size: no effect
  EXPECT_FALSE(budget.TryAcquire("alice", 50'000'000, 1'000, 10, 80'000'000));
}

TEST(AutoDownloadBudgetTest, ARefundGivesBackTheCountToo) {
  AutoDownloadBudget budget;
  EXPECT_TRUE(budget.TryAcquire("alice", 10, 1'000, 1, 1'000));
  EXPECT_FALSE(budget.TryAcquire("alice", 10, 1'000, 1, 1'000));
  budget.Refund("alice", 10);
  EXPECT_TRUE(budget.TryAcquire("alice", 10, 1'000, 1, 1'000));
}

TEST(AttachmentLimitsTest, EachKindHasItsOwnSendLimit) {
  AttachmentLimits limits;
  limits.max_image_bytes = 1;
  limits.max_original_image_bytes = 2;
  limits.max_file_bytes = 3;
  limits.max_video_bytes = 4;
  limits.max_audio_bytes = 5;
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "image/jpeg", /*image_is_shrunk=*/true), 1u);
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "image/jpeg", /*image_is_shrunk=*/false), 2u);
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "image/gif", false), 2u);
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "application/pdf", false), 3u);
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "application/octet-stream", false), 3u);
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "video/mp4", false), 4u);
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "video/quicktime", false), 4u);
  EXPECT_EQ(MaxAttachmentSendBytes(limits, "audio/mpeg", false), 5u);
}

TEST(AttachmentLimitsTest, LimitIsWrittenInDecimalMegabytes) {
  EXPECT_EQ(FormatAttachmentLimit(4'000'000), "4 MB");
  EXPECT_EQ(FormatAttachmentLimit(kLargeAttachmentMaxBytes), "50 MB");
  EXPECT_EQ(FormatAttachmentLimit(100'000'000), "100 MB");
}

TEST(AttachmentSendLimiterTest, TenPerMinuteThenBlockedUntilTheWindowMoves) {
  AttachmentSendLimiter limiter;
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(limiter.TryAcquire(1000 + i * 100, 10)) << i;
  }
  EXPECT_FALSE(limiter.TryAcquire(2500, 10));
  EXPECT_FALSE(limiter.TryAcquire(60'999, 10)) << "the first send is still inside the minute";
  EXPECT_TRUE(limiter.TryAcquire(61'000, 10)) << "the first send has left the window";
  EXPECT_FALSE(limiter.TryAcquire(61'001, 10)) << "but the other nine are still in it";
}

TEST(AutoDownloadBudgetTest, ASenderOverItsCountIsHeldBackAndAnotherIsNot) {
  AutoDownloadBudget budget;
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(budget.TryAcquire("alice", 100, 1000 + i, 10, 20'000'000)) << i;
  }
  EXPECT_FALSE(budget.TryAcquire("alice", 100, 2000, 10, 20'000'000));
  EXPECT_TRUE(budget.TryAcquire("bob", 100, 2000, 10, 20'000'000));
  EXPECT_TRUE(budget.TryAcquire("alice", 100, 61'001, 10, 20'000'000)) << "a minute later";
}

TEST(AutoDownloadBudgetTest, BytesPerMinuteAreCountedPerSender) {
  AutoDownloadBudget budget;
  EXPECT_TRUE(budget.TryAcquire("alice", 15'000'000, 0, 10, 20'000'000));
  EXPECT_FALSE(budget.TryAcquire("alice", 6'000'000, 1, 10, 20'000'000));
  EXPECT_TRUE(budget.TryAcquire("alice", 5'000'000, 2, 10, 20'000'000));
  EXPECT_TRUE(budget.TryAcquire("bob", 20'000'000, 3, 10, 20'000'000));
  EXPECT_FALSE(budget.TryAcquire("bob", 1, 4, 10, 20'000'000));
}

// #474: attachments live on the files relay (#465), so the warning reads its byte quota, not /blobs.
TEST(BlobSpaceTest, PercentIsOfTheFilesRelaysQuotaAndHasNoDenominatorGuess) {
  EXPECT_EQ(BlobSpaceUsedPercent({.bytes_used = 850, .quota_bytes = 1'000}), 85);
  EXPECT_EQ(BlobSpaceUsedPercent({.bytes_used = 1'800, .quota_bytes = 2'000}), 90);
  EXPECT_EQ(BlobSpaceUsedPercent({.bytes_used = 2'500, .quota_bytes = 2'000}), 100) << "over the quota reads full";
  EXPECT_EQ(BlobSpaceUsedPercent({.bytes_used = 500}), 0) << "no quota known: no percent";
}

TEST(BlobSpaceTest, WarnLevelsAreEightyAndNinety) {
  const AttachmentLimits limits;
  EXPECT_EQ(BlobSpaceWarnLevel(79, limits), 0);
  EXPECT_EQ(BlobSpaceWarnLevel(80, limits), 80);
  EXPECT_EQ(BlobSpaceWarnLevel(89, limits), 80);
  EXPECT_EQ(BlobSpaceWarnLevel(90, limits), 90);
  EXPECT_EQ(BlobSpaceWarnLevel(100, limits), 90);
}

TEST(BlobSpaceTest, EachLevelIsSaidOnceAndTheCheckIsDailyOrAfterAFullAnswer) {
  const AttachmentLimits limits;
  BlobSpaceMonitor monitor;
  EXPECT_EQ(monitor.NextWarning(50, limits), 0);
  EXPECT_EQ(monitor.NextWarning(82, limits), 80);
  EXPECT_EQ(monitor.NextWarning(85, limits), 0) << "80 was said";
  EXPECT_EQ(monitor.NextWarning(91, limits), 90);
  EXPECT_EQ(monitor.NextWarning(99, limits), 0) << "90 was said";

  BlobSpaceMonitor timing;
  constexpr int64_t kDay = 24LL * 60LL * 60LL * 1000LL;
  EXPECT_TRUE(timing.ShouldCheck(1'000, false));
  EXPECT_FALSE(timing.ShouldCheck(1'000 + kDay - 1, false));
  EXPECT_TRUE(timing.ShouldCheck(2'000, true)) << "a 'space full' answer checks again at once";
  EXPECT_TRUE(timing.ShouldCheck(2'000 + kDay, false));
}

TEST(AttachmentMimeTest, TypeTableCoversTheFourKinds) {
  EXPECT_EQ(AttachmentMimeFromFilename("a.PNG"), "image/png");
  EXPECT_EQ(AttachmentMimeFromFilename("a.jpeg"), "image/jpeg");
  EXPECT_EQ(AttachmentMimeFromFilename("a.pdf"), "application/pdf");
  EXPECT_EQ(AttachmentMimeFromFilename("clip.mp4"), "video/mp4");
  EXPECT_EQ(AttachmentMimeFromFilename("clip.MOV"), "video/quicktime");
  EXPECT_EQ(AttachmentMimeFromFilename("song.mp3"), "audio/mpeg");
  EXPECT_EQ(AttachmentMimeFromFilename("song.m4a"), "audio/mp4");
  EXPECT_EQ(AttachmentMimeFromFilename("take.wav"), "audio/wav");
  EXPECT_EQ(AttachmentMimeFromFilename("voice-1000.ogg"), "audio/ogg");
  // Not images for us: a file row, never a broken picture.
  EXPECT_EQ(AttachmentMimeFromFilename("photo.heic"), "application/octet-stream");
  EXPECT_EQ(AttachmentMimeFromFilename("noext"), "application/octet-stream");
}

TEST(AttachmentMimeTest, AFileWithNoNameIsTypedByItsFirstBytes) {
  const auto bytes = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
  const auto raw = [](const char* s, const size_t n) { return std::vector<uint8_t>(s, s + n); };
  EXPECT_EQ(AttachmentExtensionFromContent(bytes(std::string("\0\0\0\x18" "ftypqt  \0\0\0\0", 16))), "mov");
  EXPECT_EQ(AttachmentExtensionFromContent(bytes(std::string("\0\0\0\x18" "ftypisom\0\0\0\0", 16))), "mp4");
  EXPECT_EQ(AttachmentExtensionFromContent(bytes(std::string("\0\0\0\x18" "ftypM4A \0\0\0\0", 16))), "m4a");
  EXPECT_EQ(AttachmentExtensionFromContent(bytes("\x1a\x45\xdf\xa3 more")), "webm");
  EXPECT_EQ(AttachmentExtensionFromContent(raw("OggS\0\2", 6)), "ogg");
  EXPECT_EQ(AttachmentExtensionFromContent(raw("RIFF\0\0\0\0WAVEfmt ", 16)), "wav");
  EXPECT_EQ(AttachmentExtensionFromContent(raw("RIFF\0\0\0\0WEBPVP8 ", 16)), "webp");
  EXPECT_EQ(AttachmentExtensionFromContent(raw("ID3\3\0", 5)), "mp3");
  EXPECT_EQ(AttachmentExtensionFromContent(bytes("%PDF-1.7")), "pdf");
  EXPECT_EQ(AttachmentExtensionFromContent({0xFF, 0xD8, 0xFF, 0xE0}), "jpg");
  EXPECT_EQ(AttachmentExtensionFromContent(bytes("GIF89a..")), "gif");
  EXPECT_EQ(AttachmentExtensionFromContent(raw("MZ\x90\0 an executable", 16)), "") << "unknown stays unnamed";
  EXPECT_EQ(AttachmentExtensionFromContent({}), "");
}

TEST(AttachmentMimeTest, NewTypesKeepTheirExtensionOnDisk) {
  EXPECT_EQ(AttachmentExtensionFromMime("video/quicktime", "x.exe"), "mov") << "mapped mimes ignore the filename";
  EXPECT_EQ(AttachmentExtensionFromMime("audio/mpeg", "song.mp3"), "mp3");
  EXPECT_EQ(AttachmentExtensionFromMime("audio/mp4", "a.m4a"), "m4a");
  EXPECT_EQ(AttachmentExtensionFromMime("audio/wav", "a.wav"), "wav");
  EXPECT_EQ(AttachmentExtensionFromMime("audio/ogg", "voice-9.ogg"), "ogg");
  EXPECT_EQ(AttachmentExtensionFromMime("audio/ogg", "evil.exe"), "") << "an executable name is never kept";
}

TEST(ChatAttachmentPrepareTest, BytesAreCheckedAgainstTheCallersLimit) {
  const std::vector<uint8_t> bytes(100, 7);
  auto ok = PrepareChatAttachmentFromBytes(bytes, "note.txt", "", 100);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(ok->fields.mime, "text/plain") << "mime from the file name when none is given";
  EXPECT_EQ(ok->fields.byte_length, 100u);
  EXPECT_FALSE(PrepareChatAttachmentFromBytes(bytes, "note.txt", "", 99));
  EXPECT_FALSE(PrepareChatAttachmentFromBytes({}, "note.txt", "", 99));
  auto forced = PrepareChatAttachmentFromBytes(bytes, "weird.heic", "application/octet-stream", 100);
  ASSERT_TRUE(forced);
  EXPECT_EQ(forced->fields.mime, "application/octet-stream");
}

} // namespace
} // namespace pbr
