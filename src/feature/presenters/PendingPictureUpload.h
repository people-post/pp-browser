#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pbr {

/**
 * The picture being sent, shown at once as our own picture bubble at the end of its chat while it uploads
 * (DEV/PLAN-picture-upload-bubble.md). The row is never stored: it lives here until the send ends, and the real row
 * takes its place. One attachment is sent at a time, so there is at most one. All changes are pure functions below;
 * an event a state does not list is ignored (a thumbnail that is no longer wanted is handed back for deletion).
 */

/** The prefix of the temporary row's id ("pending-upload-<n>"): every row entry point ignores such a row. */
inline constexpr std::string_view kPendingUploadRowPrefix = "pending-upload-";
bool IsPendingUploadRowId(std::string_view message_id);

/** A picture's size in pixels. */
struct PicturePixels {
  int width = 0;
  int height = 0;
};

/** The size a picture is seen at: EXIF orientations 5..8 turn it a quarter, so width and height swap. */
PicturePixels UprightPixels(int width, int height, int exif_orientation);

/** The placeholder's proportions when the picture's head cannot be read (an Android content:// pick, for one). */
inline constexpr PicturePixels kUnknownPicturePixels{4, 3};

enum class PendingPictureState {
  None,       ///< no row
  Uploading,  ///< preparing or uploading: the disc cancels, the ring shows the percent
  Cancelling, ///< cancel tapped: the disc only spins, nothing to tap
  Failed,     ///< did not go out, a staged copy is kept: the disc retries, the corner ✕ gives it up
};

struct PendingPictureUpload {
  PendingPictureState state = PendingPictureState::None;
  std::string row_id;
  /** The chat it is sent in: the row shows only while that chat is open. */
  std::string thread_id;
  /** The 600px thumbnail (plaintext, in the profile's `picked/`); empty: the grey placeholder. */
  std::string thumb_path;
  /** The upright pixel size the bubble is laid out with. */
  PicturePixels pixels = kUnknownPicturePixels;
  /** The picture's head gave its size; false: 4:3 until the worker reads the real one. */
  bool size_known = false;
  int percent = 0;

  bool Shows() const { return state != PendingPictureState::None; }
  bool ShowsIn(const std::string& thread) const { return Shows() && thread == thread_id; }
};

/** An event's result: the new value, and a thumbnail file to delete now (no longer shown, or the send ended). */
struct PendingPictureStep {
  PendingPictureUpload next;
  std::string delete_file;
};

/** How a send ended (its callback). Failed: whether a staged copy is kept for a retry. */
enum class PictureSendOutcome { Sent, Cancelled, FailedStaged, FailedNotStaged };

/** A picture was chosen: Uploading at 0%, sized from its head (`upright`, already turned), else 4:3. */
PendingPictureUpload StartPendingPicture(uint64_t serial, const std::string& thread_id,
                                         std::optional<PicturePixels> upright);
/**
 * The worker's thumbnail gives the real (upright) size: taken while uploading when the head gave none, or when its
 * proportions differ from the head's (they disagree on the orientation): the picture is never drawn stretched.
 * A difference within rounding (a 600px thumbnail of an odd size) keeps the head's size, so the bubble does not move.
 */
PendingPictureStep OnPictureSizeRead(const PendingPictureUpload& now, PicturePixels upright);
/**
 * The thumbnail was written. Shown while uploading; deleted at once when the row is gone, the send is no longer
 * uploading, or the app is `locked` (the plaintext does not stay).
 */
PendingPictureStep OnPictureThumbnail(const PendingPictureUpload& now, const std::string& path, bool locked);
/** Upload progress: the percent never goes back. */
PendingPictureStep OnPictureProgress(const PendingPictureUpload& now, int percent);
/** The disc while uploading (preparing included: the send then stops when its upload starts and calls back). */
PendingPictureStep OnPictureCancelTapped(const PendingPictureUpload& now);
/** The send's callback. A failure after a cancel removes the row too (nothing is said). */
PendingPictureStep OnPictureSendFinished(const PendingPictureUpload& now, PictureSendOutcome outcome);
/** The disc after a failure: uploading again from 0%. */
PendingPictureStep OnPictureRetryTapped(const PendingPictureUpload& now);
/** The corner ✕ after a failure: the row goes. */
PendingPictureStep OnPictureDiscardTapped(const PendingPictureUpload& now);
/** Another attachment starts while this one shows failed: the row goes (its staged copy is left as it is). */
PendingPictureStep OnOtherAttachmentStarted(const PendingPictureUpload& now);
/** The app locked: the thumbnail is deleted and forgotten, the rest stays (the row shows the grey placeholder). */
PendingPictureStep OnAppLocked(const PendingPictureUpload& now);

/**
 * Retry / Discard on the open chat (`active_thread`): a failed voice recording takes the tap only when it is this
 * chat's and no picture bubble shows here; otherwise the tap is the staged send's (the picture bubble's).
 */
bool FailedVoiceTakesTheTap(bool voice_failed, const std::string& voice_thread, const std::string& active_thread,
                            bool picture_bubble_here);

/** The progress the bubble shows is redrawn in steps of this many percent (and at 100). */
inline constexpr int kPendingProgressStep = 5;
/** Whether `percent` is to be drawn when `drawn` is shown now. */
bool PendingProgressDue(int drawn, int percent);

} // namespace pbr
