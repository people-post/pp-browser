#include "foundation/error/AppError.h"
#include "lib/net/BlobClient.h"
#include "lib/net/BlobQuota.h"
#include "lib/net/testing/MockOrgBackendClients.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace pbr {
namespace {

RelayFileRecord Complete(const std::string& id, const uint64_t bytes, const std::string& completed_at) {
  RelayFileRecord file;
  file.blob_id = id;
  file.status = "complete";
  file.byte_length = bytes;
  file.created_at = completed_at;
  file.completed_at = completed_at;
  return file;
}

std::string Day(const int day) {
  char text[32];
  std::snprintf(text, sizeof(text), "2026-10-%02dT00:00:00.000Z", day);
  return text;
}

/** What the relay's 429 looks like once HttpBlobClient::FileStart has noted the size it asked for. */
Error FullSpace(const uint64_t asked) {
  return AppError::Blob(Err::Blob::QuotaExceeded,
                        "HTTP 429 QUOTA_FILE_BYTES: File storage quota exceeded byte_length=" + std::to_string(asked));
}

/**
 * The whole recovery the chat runs on a full space (BlobQuotaRecoveryFlow without its dialog): plan, delete every
 * planned id, upload once more; the second answer is final.
 */
Roe<RelayFileUpload> UploadWithRecovery(MockBlobClient& blob, const std::string& body) {
  auto first = UploadRelayFileBytes(blob, "relay:u1", body);
  if (first || !IsBlobQuotaError(first.error())) {
    return first;
  }
  if (BlobQuotaTierOf(first.error()) != BlobTier::Files) {
    return first.error();
  }
  auto plan = PlanOldestRelayFileDeletion(blob, "relay:u1", RelayFileQuotaNeededBytes(first.error()));
  if (!plan) {
    return plan.error();
  }
  for (const std::string& id : plan->blob_ids_to_delete) {
    if (auto deleted = DeleteRelayFile(blob, "relay:u1", id); !deleted) {
      return deleted.error();
    }
  }
  return UploadRelayFileBytes(blob, "relay:u1", body);
}

TEST(BlobQuotaFilesTest, TheFullSpaceCodeNamesTheFilesTierAndTheSize) {
  EXPECT_EQ(BlobQuotaTierOf(FullSpace(5000)), BlobTier::Files);
  EXPECT_TRUE(IsBlobQuotaError(FullSpace(5000)));
  EXPECT_EQ(RelayFileQuotaNeededBytes(FullSpace(5000)), 5000u);
  EXPECT_EQ(RelayFileErrorCode(FullSpace(5000)), "QUOTA_FILE_BYTES");
  EXPECT_EQ(RelayFileQuotaNeededBytes(Error("HTTP 429 QUOTA_FILE_BYTES: File storage quota exceeded")), 0u);
  // The /blobs texts keep their tiers.
  EXPECT_EQ(BlobQuotaTierOf(AppError::Blob(Err::Blob::QuotaExceeded, "HTTP 429: Small file quota exceeded")),
            BlobTier::Small);
}

TEST(BlobQuotaFilesTest, AFullSpaceDeletesTheOldestUntilItFitsAndUploadsOnceMore) {
  MockBlobClient blob;
  RelayFileListResult listed;
  listed.files = {Complete("newest", 400, Day(20)), Complete("middle", 300, Day(10)), Complete("oldest", 200, Day(1))};
  listed.usage.bytes_used = 900;
  listed.usage.quota_bytes = 1000;
  blob.SetFileListResult(listed);
  blob.FailNextFileStarts(FullSpace(450), 1);

  auto uploaded = UploadWithRecovery(blob, std::string(450, 'c'));
  ASSERT_TRUE(uploaded) << uploaded.error().message;
  // 900 used + 450 > 1000: 200 (oldest) leaves 700 + 450 > 1000; with 300 (middle) 400 + 450 fits.
  EXPECT_EQ(blob.DeletedFileIds(), (std::vector<std::string>{"oldest", "middle"}));
  EXPECT_EQ(blob.FileListFilters(), (std::vector<std::string>{"complete"}));
  EXPECT_EQ(blob.FileStartCallCount(), 2u);
}

TEST(BlobQuotaFilesTest, TheOldestByCompletionGoesFirstEvenAmongFiveHundred) {
  MockBlobClient blob;
  RelayFileListResult listed;
  for (int i = 0; i < 500; ++i) { // the relay lists the newest 500, newest first
    char stamp[40];
    std::snprintf(stamp, sizeof(stamp), "2026-10-%02dT%02d:%02d:00.000Z", 1 + (499 - i) / 60 % 28, (499 - i) / 24 % 24,
                  (499 - i) % 60);
    listed.files.push_back(Complete("f" + std::to_string(i), 1, stamp));
  }
  listed.usage.bytes_used = 1000;
  listed.usage.quota_bytes = 1000;
  blob.SetFileListResult(listed);
  auto plan = PlanOldestRelayFileDeletion(blob, "relay:u1", 1);
  ASSERT_TRUE(plan) << plan.error().message;
  ASSERT_EQ(plan->blob_ids_to_delete.size(), 1u);
  std::string oldest = listed.files.front().completed_at;
  std::string oldest_id = listed.files.front().blob_id;
  for (const auto& file : listed.files) {
    if (file.completed_at < oldest) {
      oldest = file.completed_at;
      oldest_id = file.blob_id;
    }
  }
  EXPECT_EQ(plan->blob_ids_to_delete.front(), oldest_id);
  EXPECT_EQ(plan->full_tier, BlobTier::Files);
  EXPECT_EQ(plan->file_usage.quota_bytes, 1000u);
}

TEST(BlobQuotaFilesTest, WhenEvenEveryListedFileWouldNotFreeEnoughNothingIsDeletedAndItIsReportedFull) {
  MockBlobClient blob;
  RelayFileListResult listed;
  listed.files = {Complete("a", 10, Day(2)), Complete("b", 10, Day(1))};
  listed.usage.bytes_used = 1000; // mostly pending uploads, which this plan does not touch
  listed.usage.quota_bytes = 1000;
  blob.SetFileListResult(listed);
  blob.SetFileStartError(FullSpace(500));

  auto uploaded = UploadWithRecovery(blob, std::string(500, 'c'));
  ASSERT_FALSE(uploaded);
  EXPECT_TRUE(IsBlobQuotaError(uploaded.error()));
  EXPECT_TRUE(blob.DeletedFileIds().empty());
  EXPECT_EQ(blob.FileListFilters().size(), 1u) << "listed once, no loop";
  EXPECT_EQ(blob.FileStartCallCount(), 1u);
}

TEST(BlobQuotaFilesTest, StillFullAfterTheRecoveryIsTheFinalAnswer) {
  MockBlobClient blob;
  RelayFileListResult listed;
  listed.files = {Complete("a", 600, Day(1))};
  listed.usage.bytes_used = 900;
  listed.usage.quota_bytes = 1000;
  blob.SetFileListResult(listed);
  blob.SetFileStartError(FullSpace(450)); // another device filled it again meanwhile

  auto uploaded = UploadWithRecovery(blob, std::string(450, 'c'));
  ASSERT_FALSE(uploaded);
  EXPECT_TRUE(IsBlobQuotaError(uploaded.error()));
  EXPECT_EQ(blob.DeletedFileIds(), (std::vector<std::string>{"a"}));
  EXPECT_EQ(blob.FileListFilters().size(), 1u);
  EXPECT_EQ(blob.FileStartCallCount(), 2u);
}

TEST(BlobQuotaFilesTest, AnEmptyListHasNothingToDelete) {
  MockBlobClient blob;
  auto plan = PlanOldestRelayFileDeletion(blob, "relay:u1", 10);
  ASSERT_FALSE(plan);
  EXPECT_EQ(plan.error().code, static_cast<int32_t>(Err::Blob::NothingToDelete));
}

// A confirmed recovery deletes each id on its plan's endpoint: a profile photo's /blobs plan on /blobs/delete, an
// attachment's Files plan on /files/delete.
TEST(BlobQuotaFilesTest, EachPlanDeletesOnItsTiersEndpoint) {
  MockBlobClient blob;
  BlobListResult blobs;
  RelayBlobRecord old_file;
  old_file.blob_id = "small-1";
  old_file.status = BlobStatus::Retained;
  old_file.tier = BlobTier::Small;
  old_file.purpose = BlobPurpose::File;
  old_file.created_at = Day(1);
  blobs.blobs = {old_file};
  blob.SetListResult(blobs);
  auto small = PlanOldestRelayBlobDeletion(blob, "relay:u1", "", BlobTier::Small);
  ASSERT_TRUE(small) << small.error().message;
  ASSERT_EQ(small->full_tier, BlobTier::Small);
  for (const std::string& id : small->blob_ids_to_delete) {
    ASSERT_TRUE(DeleteRelayQuotaItem(blob, "relay:u1", id, small->full_tier));
  }
  EXPECT_EQ(blob.DeletedBlobIds(), (std::vector<std::string>{"small-1"}));
  EXPECT_TRUE(blob.DeletedFileIds().empty());

  RelayFileListResult files;
  files.files = {Complete("file-1", 100, Day(2))};
  files.usage.bytes_used = 1000;
  files.usage.quota_bytes = 1000;
  blob.SetFileListResult(files);
  auto plan = PlanOldestRelayFileDeletion(blob, "relay:u1", 50);
  ASSERT_TRUE(plan) << plan.error().message;
  for (const std::string& id : plan->blob_ids_to_delete) {
    ASSERT_TRUE(DeleteRelayQuotaItem(blob, "relay:u1", id, plan->full_tier));
  }
  EXPECT_EQ(blob.DeletedFileIds(), (std::vector<std::string>{"file-1"}));
  EXPECT_EQ(blob.DeletedBlobIds().size(), 1u) << "the files plan never touched /blobs";
}

TEST(BlobQuotaFilesTest, DeleteRelayFileUsesTheFilesRelay) {
  MockBlobClient blob;
  ASSERT_TRUE(DeleteRelayFile(blob, "relay:u1", "f1"));
  EXPECT_EQ(blob.DeletedFileIds(), (std::vector<std::string>{"f1"}));
  EXPECT_TRUE(blob.DeletedBlobIds().empty());
  EXPECT_FALSE(DeleteRelayFile(blob, "", "f1"));
  EXPECT_FALSE(DeleteRelayFile(blob, "relay:u1", ""));
}

} // namespace
} // namespace pbr
