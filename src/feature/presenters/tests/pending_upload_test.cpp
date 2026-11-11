// The picture being sent as a temporary bubble at the end of its chat (DEV/PLAN-picture-upload-bubble.md, step 1):
// one case per row of the plan's transition table, then the extra cases its step 3 lists.
#include "feature/presenters/PendingUpload.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

using State = PendingUploadState;

PendingUpload Uploading(const int percent = 0, const std::string& thumb = "/p/thumb.jpg") {
  PendingUpload upload = StartPendingUpload(7, "t1", PicturePixels{4000, 3000});
  upload.thumb_path = thumb;
  upload.percent = percent;
  return upload;
}

PendingUpload In(const State state) {
  PendingUpload upload = Uploading(40);
  upload.state = state;
  return upload;
}

// None + picture chosen, head read → Uploading 0%, no thumbnail, sized from the head.
TEST(PendingUploadTest, AChosenPictureWithAReadableHeadUploadsAtZeroSizedFromIt) {
  const PendingUpload upload = StartPendingUpload(3, "t1", PicturePixels{1200, 1600});
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
TEST(PendingUploadTest, AnUnreadableHeadUploadsWithAFourByThreePlaceholder) {
  const PendingUpload upload = StartPendingUpload(1, "t1", std::nullopt);
  EXPECT_EQ(upload.state, State::Uploading);
  EXPECT_FALSE(upload.size_known);
  EXPECT_EQ(upload.pixels.width, 4);
  EXPECT_EQ(upload.pixels.height, 3);
}

// Uploading + the worker's real size → the real proportions (only when the head gave none).
TEST(PendingUploadTest, TheWorkersSizeReplacesThePlaceholderOnlyWhenTheHeadGaveNone) {
  const PendingUpload unknown = StartPendingUpload(1, "t1", std::nullopt);
  const PendingUploadStep sized = OnUploadSizeRead(unknown, {900, 1600});
  EXPECT_TRUE(sized.next.size_known);
  EXPECT_EQ(sized.next.pixels.width, 900);
  EXPECT_EQ(sized.next.pixels.height, 1600);

  const PendingUploadStep kept = OnUploadSizeRead(Uploading(), {600, 451}); // 4000 x 3000 within rounding
  EXPECT_EQ(kept.next.pixels.width, 4000); // the head's size stays: the bubble does not change size
  EXPECT_EQ(kept.next.pixels.height, 3000);
}

// The head and the thumbnail disagree on the orientation: the thumbnail (what is drawn) wins, nothing is stretched.
TEST(PendingUploadTest, AThumbnailWhoseProportionsDifferFromTheHeadWins) {
  const PendingUploadStep turned = OnUploadSizeRead(Uploading(), {450, 600});
  EXPECT_EQ(turned.next.pixels.width, 450);
  EXPECT_EQ(turned.next.pixels.height, 600);
  EXPECT_TRUE(turned.next.size_known);
  const PendingUploadStep other = OnUploadSizeRead(Uploading(), {600, 600});
  EXPECT_EQ(other.next.pixels.width, 600);
}

// A failed voice recording of another chat must not take the open chat's Retry / Discard (the picture bubble's).
TEST(PendingUploadTest, AFailedVoiceTakesRetryAndDiscardOnlyInItsOwnChatWithoutABubble) {
  EXPECT_TRUE(FailedVoiceTakesTheTap(true, "a", "a", /*picture_bubble_here=*/false));
  EXPECT_FALSE(FailedVoiceTakesTheTap(true, "a", "b", false)) << "another chat's recording";
  EXPECT_FALSE(FailedVoiceTakesTheTap(true, "a", "a", /*picture_bubble_here=*/true)) << "the bubble wins";
  EXPECT_FALSE(FailedVoiceTakesTheTap(false, "", "a", false));
  EXPECT_FALSE(FailedVoiceTakesTheTap(true, "", "", false));
}

// Uploading + thumbnail written → Uploading with the thumbnail.
TEST(PendingUploadTest, AWrittenThumbnailIsShownWhileUploading) {
  const PendingUploadStep step = OnUploadThumbnail(Uploading(0, ""), "/p/t.jpg", /*locked=*/false);
  EXPECT_EQ(step.next.state, State::Uploading);
  EXPECT_EQ(step.next.thumb_path, "/p/t.jpg");
  EXPECT_TRUE(step.delete_file.empty());
}

// Uploading + progress N → N, never back.
TEST(PendingUploadTest, ProgressOnlyGoesUp) {
  PendingUploadStep step = OnUploadProgress(Uploading(), 37);
  EXPECT_EQ(step.next.percent, 37);
  step = OnUploadProgress(step.next, 12);
  EXPECT_EQ(step.next.percent, 37);
  step = OnUploadProgress(step.next, 140);
  EXPECT_EQ(step.next.percent, 100);
}

// Uploading + disc tapped → Cancelling.
TEST(PendingUploadTest, TheDiscWhileUploadingCancels) {
  const PendingUploadStep step = OnUploadCancelTapped(Uploading(55));
  EXPECT_EQ(step.next.state, State::Cancelling);
  EXPECT_EQ(step.next.thumb_path, "/p/thumb.jpg");
}

// Uploading / Cancelling + sent → the row goes (its thumbnail deleted).
TEST(PendingUploadTest, SentRemovesTheRowFromUploadingAndFromCancelling) {
  for (const State state : {State::Uploading, State::Cancelling}) {
    const PendingUploadStep step = OnUploadSendFinished(In(state), UploadSendOutcome::Sent);
    EXPECT_FALSE(step.next.Shows());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  }
}

// Uploading / Cancelling + cancelled → the row goes.
TEST(PendingUploadTest, CancelledRemovesTheRow) {
  for (const State state : {State::Uploading, State::Cancelling}) {
    const PendingUploadStep step = OnUploadSendFinished(In(state), UploadSendOutcome::Cancelled);
    EXPECT_FALSE(step.next.Shows());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  }
}

// Uploading + failed with a staged copy → Failed (thumbnail kept).
TEST(PendingUploadTest, AFailureWithAStagedCopyShowsFailed) {
  const PendingUploadStep step = OnUploadSendFinished(Uploading(80), UploadSendOutcome::FailedStaged);
  EXPECT_EQ(step.next.state, State::Failed);
  EXPECT_EQ(step.next.thumb_path, "/p/thumb.jpg");
  EXPECT_TRUE(step.delete_file.empty());
}

// Uploading + failed with nothing staged (failed while preparing) → the row goes.
TEST(PendingUploadTest, AFailureWithNothingStagedRemovesTheRow) {
  const PendingUploadStep step = OnUploadSendFinished(Uploading(0), UploadSendOutcome::FailedNotStaged);
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
}

// Cancelling + failed (staged or not) → the row goes.
TEST(PendingUploadTest, AFailureAfterCancelRemovesTheRow) {
  for (const UploadSendOutcome outcome : {UploadSendOutcome::FailedStaged, UploadSendOutcome::FailedNotStaged}) {
    const PendingUploadStep step = OnUploadSendFinished(In(State::Cancelling), outcome);
    EXPECT_FALSE(step.next.Shows());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  }
}

// Uploading, still preparing + disc tapped → Cancelling (the send calls back once its upload starts: step 0).
TEST(PendingUploadTest, CancelWhilePreparingWaitsForTheCallback) {
  const PendingUpload preparing = StartPendingUpload(1, "t1", std::nullopt); // no thumbnail, 0%
  const PendingUploadStep step = OnUploadCancelTapped(preparing);
  EXPECT_EQ(step.next.state, State::Cancelling);
  const PendingUploadStep ended = OnUploadSendFinished(step.next, UploadSendOutcome::Cancelled);
  EXPECT_FALSE(ended.next.Shows());
}

// None (row gone) + thumbnail written late → delete it at once.
TEST(PendingUploadTest, AThumbnailForARowThatIsGoneIsDeleted) {
  const PendingUploadStep step = OnUploadThumbnail(PendingUpload{}, "/p/late.jpg", /*locked=*/false);
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/late.jpg");
}

// Failed + disc tapped → Uploading from 0%.
TEST(PendingUploadTest, RetryUploadsAgainFromZero) {
  const PendingUploadStep step = OnUploadRetryTapped(In(State::Failed));
  EXPECT_EQ(step.next.state, State::Uploading);
  EXPECT_EQ(step.next.percent, 0);
  EXPECT_EQ(step.next.thumb_path, "/p/thumb.jpg");
}

// Failed + corner ✕ → the row goes.
TEST(PendingUploadTest, DiscardRemovesTheFailedRow) {
  const PendingUploadStep step = OnUploadDiscardTapped(In(State::Failed));
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
}

// Failed + another attachment starts → the row goes.
TEST(PendingUploadTest, AnotherAttachmentRemovesTheFailedRow) {
  const PendingUploadStep step = OnOtherAttachmentStarted(In(State::Failed));
  EXPECT_FALSE(step.next.Shows());
  EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
  // Not while uploading: one send at a time, nothing else can start then.
  EXPECT_EQ(OnOtherAttachmentStarted(Uploading()).next.state, State::Uploading);
}

// Any + app locked → same state, thumbnail deleted and forgotten.
TEST(PendingUploadTest, LockingDeletesTheThumbnailAndKeepsTheState) {
  for (const State state : {State::Uploading, State::Cancelling, State::Failed}) {
    const PendingUploadStep step = OnAppLocked(In(state));
    EXPECT_EQ(step.next.state, state);
    EXPECT_TRUE(step.next.thumb_path.empty());
    EXPECT_EQ(step.delete_file, "/p/thumb.jpg");
    EXPECT_EQ(step.next.percent, 40);
  }
}

// Events a state does not list change nothing.
TEST(PendingUploadTest, UnlistedEventsAreIgnored) {
  EXPECT_EQ(OnUploadRetryTapped(Uploading()).next.state, State::Uploading);
  EXPECT_EQ(OnUploadDiscardTapped(Uploading()).next.state, State::Uploading);
  EXPECT_EQ(OnUploadCancelTapped(In(State::Failed)).next.state, State::Failed);
  EXPECT_EQ(OnUploadCancelTapped(In(State::Cancelling)).next.state, State::Cancelling);
  EXPECT_EQ(OnUploadProgress(In(State::Cancelling), 90).next.percent, 40);
  EXPECT_EQ(OnUploadSendFinished(In(State::Failed), UploadSendOutcome::Sent).next.state, State::Failed);
  EXPECT_FALSE(OnUploadSendFinished(PendingUpload{}, UploadSendOutcome::Sent).next.Shows());
}

// A thumbnail that arrives after the cancel, or while the app is locked, is deleted and not shown.
TEST(PendingUploadTest, AThumbnailWhileCancellingOrLockedIsDeleted) {
  const PendingUploadStep cancelling = OnUploadThumbnail(In(State::Cancelling), "/p/new.jpg", false);
  EXPECT_EQ(cancelling.delete_file, "/p/new.jpg");
  EXPECT_EQ(cancelling.next.thumb_path, "/p/thumb.jpg");
  const PendingUploadStep locked = OnUploadThumbnail(Uploading(0, ""), "/p/new.jpg", /*locked=*/true);
  EXPECT_EQ(locked.delete_file, "/p/new.jpg");
  EXPECT_TRUE(locked.next.thumb_path.empty());
}

// Plan step 3: 100% reached, then cancel, then the send calls back sent → the row goes.
TEST(PendingUploadTest, CancelAtHundredPercentThenSentRemovesTheRow) {
  PendingUploadStep step = OnUploadProgress(Uploading(), 100);
  step = OnUploadCancelTapped(step.next);
  ASSERT_EQ(step.next.state, State::Cancelling);
  step = OnUploadSendFinished(step.next, UploadSendOutcome::Sent);
  EXPECT_FALSE(step.next.Shows());
}

// Plan step 3: an EXIF orientation 6 JPEG (stored 4000 x 3000, seen standing) is laid out standing, as its thumbnail.
TEST(PendingUploadTest, AnOrientationSixPhotoIsLaidOutStandingLikeItsThumbnail) {
  const PicturePixels upright = UprightPixels(4000, 3000, 6);
  EXPECT_EQ(upright.width, 3000);
  EXPECT_EQ(upright.height, 4000);
  const PendingUpload upload = StartPendingUpload(1, "t1", upright);
  // The thumbnail (written upright, long edge 600) has the same proportions.
  EXPECT_EQ(upload.pixels.width * 600, upload.pixels.height * 450);
  for (const int orientation : {1, 2, 3, 4}) {
    EXPECT_EQ(UprightPixels(4000, 3000, orientation).width, 4000) << orientation;
  }
  for (const int orientation : {5, 7, 8}) {
    EXPECT_EQ(UprightPixels(4000, 3000, orientation).width, 3000) << orientation;
  }
}

TEST(PendingUploadTest, OnlyPendingRowIdsAreRecognised) {
  EXPECT_TRUE(IsPendingUploadRowId("pending-upload-1"));
  EXPECT_FALSE(IsPendingUploadRowId("msg-1"));
  EXPECT_FALSE(IsPendingUploadRowId("streaming-x"));
  EXPECT_FALSE(IsPendingUploadRowId(""));
}

TEST(PendingUploadTest, ProgressIsRedrawnInFivePercentStepsAndAtTheEnd) {
  EXPECT_FALSE(PendingProgressDue(0, 4));
  EXPECT_TRUE(PendingProgressDue(0, 5));
  EXPECT_FALSE(PendingProgressDue(5, 9));
  EXPECT_TRUE(PendingProgressDue(97, 100));
  EXPECT_FALSE(PendingProgressDue(100, 100));
}

// DEV/PLAN-attachment-bubbles-multi-pick.md step 1: the items in a queue, events by item id.
PendingUpload Item(const uint64_t serial, const std::string& thread = "t1") {
  PendingUpload item = StartPendingUpload(serial, thread, PicturePixels{800, 600});
  item.thumb_path = "/p/thumb-" + std::to_string(serial) + ".jpg";
  return item;
}

TEST(PendingUploadQueueTest, ItemsAreAddedInOrderAndFoundById) {
  PendingUploadQueue queue;
  EXPECT_TRUE(queue.Empty());
  queue.Add(Item(1));
  queue.Add(Item(2, "t2"));
  queue.Add(PendingUpload{}); // nothing to show: not added
  ASSERT_EQ(queue.Items().size(), 2u);
  EXPECT_EQ(queue.Items()[0].row_id, "pending-upload-1");
  EXPECT_EQ(queue.Items()[1].row_id, "pending-upload-2");
  ASSERT_NE(queue.Find("pending-upload-2"), nullptr);
  EXPECT_EQ(queue.Find("pending-upload-9"), nullptr);
  EXPECT_TRUE(queue.ShowsIn("t2"));
  EXPECT_FALSE(queue.ShowsIn("t3"));
  EXPECT_EQ(queue.InFlightIn("t1")->row_id, "pending-upload-1");
}

TEST(PendingUploadQueueTest, CancelRetryAndDiscardActOnTheItemTheyName) {
  PendingUploadQueue queue;
  queue.Add(Item(1));
  queue.Add(Item(2));
  EXPECT_TRUE(queue.Apply("pending-upload-2", OnUploadCancelTapped).empty());
  EXPECT_EQ(queue.Find("pending-upload-2")->state, PendingUploadState::Cancelling);
  EXPECT_EQ(queue.Find("pending-upload-1")->state, PendingUploadState::Uploading) << "the other is untouched";

  (void)queue.Apply("pending-upload-1", [](const PendingUpload& now) {
    return OnUploadSendFinished(now, UploadSendOutcome::FailedStaged);
  });
  ASSERT_EQ(queue.Find("pending-upload-1")->state, PendingUploadState::Failed);
  (void)queue.Apply("pending-upload-1", OnUploadRetryTapped);
  EXPECT_EQ(queue.Find("pending-upload-1")->state, PendingUploadState::Uploading);

  (void)queue.Apply("pending-upload-1", [](const PendingUpload& now) {
    return OnUploadSendFinished(now, UploadSendOutcome::FailedStaged);
  });
  const std::vector<std::string> gone = queue.Apply("pending-upload-1", OnUploadDiscardTapped);
  EXPECT_EQ(gone, (std::vector<std::string>{"/p/thumb-1.jpg"}));
  EXPECT_EQ(queue.Find("pending-upload-1"), nullptr) << "a discarded item leaves the queue";
  EXPECT_TRUE(queue.Apply("pending-upload-7", OnUploadDiscardTapped).empty()) << "no such item: nothing";
  ASSERT_EQ(queue.Items().size(), 1u);
}

TEST(PendingUploadQueueTest, AnotherAttachmentMakesAFailedItemGiveWay) {
  PendingUploadQueue queue;
  queue.Add(Item(1));
  (void)queue.Apply("pending-upload-1", [](const PendingUpload& now) {
    return OnUploadSendFinished(now, UploadSendOutcome::FailedStaged);
  });
  queue.Add(Item(2)); // uploading: not affected
  const std::vector<std::string> gone = queue.ApplyAll(OnOtherAttachmentStarted);
  EXPECT_EQ(gone, (std::vector<std::string>{"/p/thumb-1.jpg"}));
  ASSERT_EQ(queue.Items().size(), 1u);
  EXPECT_EQ(queue.Items()[0].row_id, "pending-upload-2");
}

TEST(PendingUploadQueueTest, ClearHandsBackEveryThumbnail) {
  PendingUploadQueue queue;
  queue.Add(Item(1));
  queue.Add(Item(2));
  EXPECT_EQ(queue.Clear(), (std::vector<std::string>{"/p/thumb-1.jpg", "/p/thumb-2.jpg"}));
  EXPECT_TRUE(queue.Empty());
}

TEST(PendingUploadQueueTest, AnItemKnowsItsKind) {
  EXPECT_EQ(StartPendingUpload(1, "t", std::nullopt).kind, PendingUploadKind::Picture);
  EXPECT_EQ(StartPendingUpload(1, "t", std::nullopt, PendingUploadKind::File).kind, PendingUploadKind::File);
}

TEST(PendingUploadQueueTest, ThePreparationNamesTheFileAndItsSize) {
  PendingUpload file = StartPendingUpload(1, "t", std::nullopt, PendingUploadKind::File);
  file.title = "picked name";
  const PendingUploadStep step = OnUploadDetails(file, "report.pdf", "2.4 MB");
  EXPECT_EQ(step.next.title, "report.pdf");
  EXPECT_EQ(step.next.size_text, "2.4 MB");
  EXPECT_EQ(OnUploadDetails(file, "", "1 KB").next.title, "picked name") << "no name given: the old one stays";
  EXPECT_FALSE(OnUploadDetails(PendingUpload{}, "x", "1 KB").next.Shows());
}

// A retry that fails while the staged copy is still there (offline, small or large file alike: the resume never
// deletes it) keeps the bubble Failed with the same record, and a later retry can still succeed. Cancelling a retry
// (a tap reaching the disc) also leaves it Failed. Only a copy that is gone removes it.
TEST(PendingUploadQueueTest, AFailedRetryKeepsTheBubbleFailedWhileItsCopyIsStaged) {
  PendingUpload failed = StartPendingUpload(1, "t", PicturePixels{800, 600}, PendingUploadKind::File);
  failed.state = PendingUploadState::Failed;
  failed.record_id = "rec-1";
  failed.thumb_path = "/p/t.jpg";
  PendingUpload retrying = OnUploadRetryTapped(failed).next;
  ASSERT_EQ(retrying.state, PendingUploadState::Uploading);
  EXPECT_EQ(retrying.record_id, "rec-1") << "a retry keeps its staged copy";
  PendingUploadStep again = OnUploadResumeFinished(retrying, /*sent=*/false, /*still_staged=*/true);
  EXPECT_EQ(again.next.state, PendingUploadState::Failed);
  EXPECT_EQ(again.next.record_id, "rec-1");
  EXPECT_TRUE(again.delete_file.empty());
  // Retry once more, and this time it goes.
  const PendingUploadStep sent = OnUploadResumeFinished(OnUploadRetryTapped(again.next).next, true, true);
  EXPECT_FALSE(sent.next.Shows());
  EXPECT_EQ(sent.delete_file, "/p/t.jpg");
  // A cancel during the retry: the copy stays, so does the failed bubble.
  const PendingUpload cancelling = OnUploadCancelTapped(retrying).next;
  EXPECT_EQ(OnUploadResumeFinished(cancelling, false, true).next.state, PendingUploadState::Failed);
}

TEST(PendingUploadQueueTest, AFailedRetryWhoseCopyIsGoneRemovesTheBubble) {
  PendingUpload retrying = StartPendingUpload(1, "t", std::nullopt, PendingUploadKind::File);
  retrying.record_id = "rec-1";
  const PendingUploadStep gone = OnUploadResumeFinished(retrying, /*sent=*/false, /*still_staged=*/false);
  EXPECT_FALSE(gone.next.Shows());
}

} // namespace
} // namespace pbr
