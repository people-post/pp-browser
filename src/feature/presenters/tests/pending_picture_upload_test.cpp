// The picture being sent as a temporary bubble at the end of its chat (DEV/PLAN-picture-upload-bubble.md, step 1):
// one case per row of the plan's transition table, then the extra cases its step 3 lists.
#include "feature/presenters/PendingPictureUpload.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

using State = PendingPictureState;

PendingPictureUpload Uploading(const int percent = 0, const std::string& thumb = "/p/thumb.jpg") {
  PendingPictureUpload upload = StartPendingPicture(7, "t1", PicturePixels{4000, 3000});
  upload.thumb_path = thumb;
  upload.percent = percent;
  return upload;
}

PendingPictureUpload In(const State state) {
  PendingPictureUpload upload = Uploading(40);
  upload.state = state;
  return upload;
}

// None + picture chosen, head read → Uploading 0%, no thumbnail, sized from the head.
TEST(PendingPictureUploadTest, AChosenPictureWithAReadableHeadUploadsAtZeroSizedFromIt) {
  const PendingPictureUpload upload = StartPendingPicture(3, "t1", PicturePixels{1200, 1600});
  EXPECT_EQ(upload.state, State::Uploading);
  EXPECT_EQ(upload.row_id, "pending-upload-3");
  EXPECT_EQ(upload.thread_id, "t1");
  EXPECT_EQ(upload.percent, 0);
  EXPECT_TRUE(upload.thumb_path.empty());
  EXPECT_TRUE(upload.size_known);
  EXPECT_EQ(upload.pixels.width, 1200);
  EXPECT_EQ(upload.pixels.height, 1600);
  EXPECT_TRUE(IsPendingUploadRowId(upload.row_id));
}

// None + picture chosen, head unreadable → Uploading 0%, 4:3 placeholder, not a failure.
TEST(PendingPictureUploadTest, AnUnreadableHeadUploadsWithAFourByThreePlaceholder) {
  const PendingPictureUpload upload = StartPendingPicture(1, "t1", std::nullopt);
  EXPECT_EQ(upload.state, State::Uploading);
  EXPECT_FALSE(upload.size_known);
  EXPECT_EQ(upload.pixels.width, 4);
  EXPECT_EQ(upload.pixels.height, 3);
}

// Uploading + the worker's real size → the real proportions (only when the head gave none).
TEST(PendingPictureUploadTest, TheWorkersSizeReplacesThePlaceholderOnlyWhenTheHeadGaveNone) {
  const PendingPictureUpload unknown = StartPendingPicture(1, "t1", std::nullopt);
  const PendingPictureStep sized = OnPictureSizeRead(unknown, {900, 1600});
  EXPECT_TRUE(sized.next.size_known);
  EXPECT_EQ(sized.next.pixels.width, 900);
  EXPECT_EQ(sized.next.pixels.height, 1600);

  const PendingPictureStep kept = OnPictureSizeRead(Uploading(), {900, 1600});
  EXPECT_EQ(kept.next.pixels.width, 4000); // the head's size stays: the bubble does not change size
  EXPECT_EQ(kept.next.pixels.height, 3000);
}

// Uploading + thumbnail written → Uploading with the thumbnail.
TEST(PendingPictureUploadTest, AWrittenThumbnailIsShownWhileUploading) {
  const PendingPictureStep step = OnPictureThumbnail(Uploading(0, ""), "/p/t.jpg", /*locked=*/false);
  EXPECT_EQ(step.next.state, State::Uploading);
  EXPECT_EQ(step.next.thumb_path, "/p/t.jpg");
  EXPECT_TRUE(step.delete_file.empty());
}

// Uploading + progress N → N, never back.
TEST(PendingPictureUploadTest, ProgressOnlyGoesUp) {
  PendingPictureStep step = OnPictureProgress(Uploading(), 37);
  EXPECT_EQ(step.next.percent, 37);
  step = OnPictureProgress(step.next, 12);
  EXPECT_EQ(step.next.percent, 37);
  step = OnPictureProgress(step.next, 140);
  EXPECT_EQ(step.next.percent, 100);
}

// Uploading + disc tapped → Cancelling.
TEST(PendingPictureUploadTest, TheDiscWhileUploadingCancels) {
  const PendingPictureStep step = OnPictureCancelTapped(Uploading(55));
  EXPECT_EQ(step.next.state, State::Cancelling);
  EXPECT_EQ(step.next.thumb_path, "/p/thumb.jpg");
}

// Uploading / Cancelling + sent → the row goes (its thumbnail deleted).
TEST(PendingPictureUploadTest, SentRemovesTheRowFromUploadingAndFromCancelling) {
  for (const State state : {State::Uploading, State::Cancelling}) {
    const PendingPictureStep step = OnPictureSendFinished(In(state), PictureSendOutcome::Sent);
    EXPECT_FALSE(step.next.Shows());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  }
}

// Uploading / Cancelling + cancelled → the row goes.
TEST(PendingPictureUploadTest, CancelledRemovesTheRow) {
  for (const State state : {State::Uploading, State::Cancelling}) {
    const PendingPictureStep step = OnPictureSendFinished(In(state), PictureSendOutcome::Cancelled);
    EXPECT_FALSE(step.next.Shows());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  }
}

// Uploading + failed with a staged copy → Failed (thumbnail kept).
TEST(PendingPictureUploadTest, AFailureWithAStagedCopyShowsFailed) {
  const PendingPictureStep step = OnPictureSendFinished(Uploading(80), PictureSendOutcome::FailedStaged);
  EXPECT_EQ(step.next.state, State::Failed);
  EXPECT_EQ(step.next.thumb_path, "/p/thumb.jpg");
  EXPECT_TRUE(step.delete_file.empty());
}

// Uploading + failed with nothing staged (failed while preparing) → the row goes.
TEST(PendingPictureUploadTest, AFailureWithNothingStagedRemovesTheRow) {
  const PendingPictureStep step = OnPictureSendFinished(Uploading(0), PictureSendOutcome::FailedNotStaged);
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
}

// Cancelling + failed (staged or not) → the row goes.
TEST(PendingPictureUploadTest, AFailureAfterCancelRemovesTheRow) {
  for (const PictureSendOutcome outcome : {PictureSendOutcome::FailedStaged, PictureSendOutcome::FailedNotStaged}) {
    const PendingPictureStep step = OnPictureSendFinished(In(State::Cancelling), outcome);
    EXPECT_FALSE(step.next.Shows());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  }
}

// Uploading, still preparing + disc tapped → Cancelling (the send calls back once its upload starts: step 0).
TEST(PendingPictureUploadTest, CancelWhilePreparingWaitsForTheCallback) {
  const PendingPictureUpload preparing = StartPendingPicture(1, "t1", std::nullopt); // no thumbnail, 0%
  const PendingPictureStep step = OnPictureCancelTapped(preparing);
  EXPECT_EQ(step.next.state, State::Cancelling);
  const PendingPictureStep ended = OnPictureSendFinished(step.next, PictureSendOutcome::Cancelled);
  EXPECT_FALSE(ended.next.Shows());
}

// None (row gone) + thumbnail written late → delete it at once.
TEST(PendingPictureUploadTest, AThumbnailForARowThatIsGoneIsDeleted) {
  const PendingPictureStep step = OnPictureThumbnail(PendingPictureUpload{}, "/p/late.jpg", /*locked=*/false);
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/late.jpg");
}

// Failed + disc tapped → Uploading from 0%.
TEST(PendingPictureUploadTest, RetryUploadsAgainFromZero) {
  const PendingPictureStep step = OnPictureRetryTapped(In(State::Failed));
  EXPECT_EQ(step.next.state, State::Uploading);
  EXPECT_EQ(step.next.percent, 0);
  EXPECT_EQ(step.next.thumb_path, "/p/thumb.jpg");
}

// Failed + corner ✕ → the row goes.
TEST(PendingPictureUploadTest, DiscardRemovesTheFailedRow) {
  const PendingPictureStep step = OnPictureDiscardTapped(In(State::Failed));
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
}

// Failed + another attachment starts → the row goes.
TEST(PendingPictureUploadTest, AnotherAttachmentRemovesTheFailedRow) {
  const PendingPictureStep step = OnOtherAttachmentStarted(In(State::Failed));
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  // Not while uploading: one send at a time, nothing else can start then.
  EXPECT_EQ(OnOtherAttachmentStarted(Uploading()).next.state, State::Uploading);
}

// Any + app locked → same state, thumbnail deleted and forgotten.
TEST(PendingPictureUploadTest, LockingDeletesTheThumbnailAndKeepsTheState) {
  for (const State state : {State::Uploading, State::Cancelling, State::Failed}) {
    const PendingPictureStep step = OnAppLocked(In(state));
    EXPECT_EQ(step.next.state, state);
    EXPECT_TRUE(step.next.thumb_path.empty());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
    EXPECT_EQ(step.next.percent, 40);
  }
}

// Events a state does not list change nothing.
TEST(PendingPictureUploadTest, UnlistedEventsAreIgnored) {
  EXPECT_EQ(OnPictureRetryTapped(Uploading()).next.state, State::Uploading);
  EXPECT_EQ(OnPictureDiscardTapped(Uploading()).next.state, State::Uploading);
  EXPECT_EQ(OnPictureCancelTapped(In(State::Failed)).next.state, State::Failed);
  EXPECT_EQ(OnPictureCancelTapped(In(State::Cancelling)).next.state, State::Cancelling);
  EXPECT_EQ(OnPictureProgress(In(State::Cancelling), 90).next.percent, 40);
  EXPECT_EQ(OnPictureSendFinished(In(State::Failed), PictureSendOutcome::Sent).next.state, State::Failed);
  EXPECT_FALSE(OnPictureSendFinished(PendingPictureUpload{}, PictureSendOutcome::Sent).next.Shows());
}

// A thumbnail that arrives after the cancel, or while the app is locked, is deleted and not shown.
TEST(PendingPictureUploadTest, AThumbnailWhileCancellingOrLockedIsDeleted) {
  const PendingPictureStep cancelling = OnPictureThumbnail(In(State::Cancelling), "/p/new.jpg", false);
  EXPECT_EQ(cancelling.delete_file, "/p/new.jpg");
  EXPECT_EQ(cancelling.next.thumb_path, "/p/thumb.jpg");
  const PendingPictureStep locked = OnPictureThumbnail(Uploading(0, ""), "/p/new.jpg", /*locked=*/true);
  EXPECT_EQ(locked.delete_file, "/p/new.jpg");
  EXPECT_TRUE(locked.next.thumb_path.empty());
}

// Plan step 3: 100% reached, then cancel, then the send calls back sent → the row goes.
TEST(PendingPictureUploadTest, CancelAtHundredPercentThenSentRemovesTheRow) {
  PendingPictureStep step = OnPictureProgress(Uploading(), 100);
  step = OnPictureCancelTapped(step.next);
  ASSERT_EQ(step.next.state, State::Cancelling);
  step = OnPictureSendFinished(step.next, PictureSendOutcome::Sent);
  EXPECT_FALSE(step.next.Shows());
}

// Plan step 3: an EXIF orientation 6 JPEG (stored 4000 x 3000, seen standing) is laid out standing, as its thumbnail.
TEST(PendingPictureUploadTest, AnOrientationSixPhotoIsLaidOutStandingLikeItsThumbnail) {
  const PicturePixels upright = UprightPixels(4000, 3000, 6);
  EXPECT_EQ(upright.width, 3000);
  EXPECT_EQ(upright.height, 4000);
  const PendingPictureUpload upload = StartPendingPicture(1, "t1", upright);
  // The thumbnail (written upright, long edge 600) has the same proportions.
  EXPECT_EQ(upload.pixels.width * 600, upload.pixels.height * 450);
  for (const int orientation : {1, 2, 3, 4}) {
    EXPECT_EQ(UprightPixels(4000, 3000, orientation).width, 4000) << orientation;
  }
  for (const int orientation : {5, 7, 8}) {
    EXPECT_EQ(UprightPixels(4000, 3000, orientation).width, 3000) << orientation;
  }
}

TEST(PendingPictureUploadTest, OnlyPendingRowIdsAreRecognised) {
  EXPECT_TRUE(IsPendingUploadRowId("pending-upload-1"));
  EXPECT_FALSE(IsPendingUploadRowId("msg-1"));
  EXPECT_FALSE(IsPendingUploadRowId("streaming-x"));
  EXPECT_FALSE(IsPendingUploadRowId(""));
}

TEST(PendingPictureUploadTest, ProgressIsRedrawnInFivePercentStepsAndAtTheEnd) {
  EXPECT_FALSE(PendingProgressDue(0, 4));
  EXPECT_TRUE(PendingProgressDue(0, 5));
  EXPECT_FALSE(PendingProgressDue(5, 9));
  EXPECT_TRUE(PendingProgressDue(97, 100));
  EXPECT_FALSE(PendingProgressDue(100, 100));
}

} // namespace
} // namespace pbr
