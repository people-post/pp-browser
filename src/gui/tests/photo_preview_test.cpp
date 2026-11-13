#include "gui/chat/PhotoPreview.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// B203: a tap shows a spinner only, no picture (the bubble holds only the small preview); the viewer gets the full one.
TEST(PhotoPreview, RmlIsASpinnerAndATapToCloseWithNoPicture) {
  const std::string rml = PhotoPreviewRml();
  EXPECT_EQ(rml.find("<img"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("photo-preview-image"), std::string::npos) << rml;
  EXPECT_NE(rml.find("photo-preview-spinner"), std::string::npos) << rml;
  EXPECT_NE(rml.find("photo_preview_close()"), std::string::npos) << rml;
}

TEST(PhotoPreview, OpeningToastOnlyForALargeNonPicture) {
  EXPECT_FALSE(OpenShowsOpeningToast(/*picture_preview=*/true, kMaxDirectAttachmentBytes + 1));
  EXPECT_FALSE(OpenShowsOpeningToast(false, kMaxDirectAttachmentBytes));
  EXPECT_TRUE(OpenShowsOpeningToast(false, kMaxDirectAttachmentBytes + 1));
}

TEST(PhotoPreview, TapMountsAndTheResultDismissesOnSuccessAndOnFailure) {
  PhotoPreviewTracker tracker;
  EXPECT_TRUE(tracker.Begin("m1"));
  EXPECT_TRUE(tracker.Up());
  EXPECT_TRUE(tracker.Finish("m1")); // success or failure: both come through Finish
  EXPECT_FALSE(tracker.Up());
  EXPECT_FALSE(tracker.Finish("m1")); // nothing left to dismiss
}

TEST(PhotoPreview, AnotherMessageResultDoesNotDismissTheNewerPreview) {
  PhotoPreviewTracker tracker;
  ASSERT_TRUE(tracker.Begin("m1"));
  ASSERT_TRUE(tracker.Begin("m2"));
  EXPECT_FALSE(tracker.Finish("m1"));
  EXPECT_TRUE(tracker.Up());
  EXPECT_TRUE(tracker.Finish("m2"));
}

TEST(PhotoPreview, NoMessageIdNoPreview) {
  PhotoPreviewTracker tracker;
  EXPECT_FALSE(tracker.Begin(""));
  EXPECT_FALSE(tracker.Up());
}

} // namespace
} // namespace pbr
