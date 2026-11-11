#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * An attachment being sent, shown at once as our own bubble at the end of its chat while it uploads
 * (DEV/PLAN-picture-upload-bubble.md, DEV/PLAN-attachment-bubbles-multi-pick.md). The row is never stored: it lives
 * here until the send ends, and the real row takes its place. Items sit in a PendingUploadQueue; one attachment is
 * sent at a time, so today it holds at most one. All changes are pure functions below; an event a state does not list
 * is ignored (a thumbnail that is no longer wanted is handed back for deletion).
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

enum class PendingUploadState {
  None,       ///< no row
  Uploading,  ///< preparing or uploading: the disc cancels, the ring shows the percent
  Cancelling, ///< cancel tapped: the disc only spins, nothing to tap
  Failed,     ///< did not go out, a staged copy is kept: the disc retries, the corner ✕ gives it up
};

/** What kind of bubble an item draws. */
enum class PendingUploadKind { Picture, Video, File };

struct PendingUpload {
  PendingUploadState state = PendingUploadState::None;
  PendingUploadKind kind = PendingUploadKind::Picture;
  /** The item's id and its row's ("pending-upload-<n>"); the bubble's events carry it. */
  std::string row_id;
  /** A file's name and size line ("2.4 MB"; empty until the preparation knows it). */
  std::string title;
  std::string size_text;
  /** The staged copy a failure kept (Retry resends it, Discard deletes it); empty while none. */
  std::string record_id;
  /** The chat it is sent in: the row shows only while that chat is open. */
  std::string thread_id;
  /** The 600px thumbnail (plaintext, in the profile's `picked/`); empty: the grey placeholder. */
  std::string thumb_path;
  /** The upright pixel size the bubble is laid out with. */
  PicturePixels pixels = kUnknownPicturePixels;
  /** The picture's head gave its size; false: 4:3 until the worker reads the real one. */
  bool size_known = false;
  int percent = 0;

  bool Shows() const { return state != PendingUploadState::None; }
  bool ShowsIn(const std::string& thread) const { return Shows() && thread == thread_id; }
};

/** An event's result: the new value, and a thumbnail file to delete now (no longer shown, or the send ended). */
struct PendingUploadStep {
  PendingUpload next;
  std::string delete_file;
};

/** How a send ended (its callback). Failed: whether a staged copy is kept for a retry. */
enum class UploadSendOutcome { Sent, Cancelled, FailedStaged, FailedNotStaged };

/** A picture was chosen: Uploading at 0%, sized from its head (`upright`, already turned), else 4:3. */
PendingUpload StartPendingUpload(uint64_t serial, const std::string& thread_id, std::optional<PicturePixels> upright,
                                 PendingUploadKind kind = PendingUploadKind::Picture);
/**
 * The worker's thumbnail gives the real (upright) size: taken while uploading when the head gave none, or when its
 * proportions differ from the head's (they disagree on the orientation): the picture is never drawn stretched.
 * A difference within rounding (a 600px thumbnail of an odd size) keeps the head's size, so the bubble does not move.
 */
PendingUploadStep OnUploadSizeRead(const PendingUpload& now, PicturePixels upright);
/**
 * The thumbnail was written. Shown while uploading; deleted at once when the row is gone, the send is no longer
 * uploading, or the app is `locked` (the plaintext does not stay).
 */
PendingUploadStep OnUploadThumbnail(const PendingUpload& now, const std::string& path, bool locked);
/** The preparation named the file it sends and its size line: a file / video bubble shows them (any shown state). */
PendingUploadStep OnUploadDetails(const PendingUpload& now, const std::string& title, const std::string& size_text);
/** Upload progress: the percent never goes back. */
PendingUploadStep OnUploadProgress(const PendingUpload& now, int percent);
/** The disc while uploading (preparing included: the send then stops when its upload starts and calls back). */
PendingUploadStep OnUploadCancelTapped(const PendingUpload& now);
/** The send's callback. A failure after a cancel removes the row too (nothing is said). */
PendingUploadStep OnUploadSendFinished(const PendingUpload& now, UploadSendOutcome outcome);
/**
 * A retry's callback (a resume of the item's staged copy). Sent: the row goes. Otherwise — failed, or cancelled by a
 * tap on the disc — the row stays Failed (retry and ✕ again) while that staged copy is still there (`still_staged`):
 * a resume never deletes it. Only when the copy is gone does the row go (the caller says "pick it again").
 */
PendingUploadStep OnUploadResumeFinished(const PendingUpload& now, bool sent, bool still_staged);
/** The disc after a failure: uploading again from 0%. */
PendingUploadStep OnUploadRetryTapped(const PendingUpload& now);
/** The corner ✕ after a failure: the row goes. */
PendingUploadStep OnUploadDiscardTapped(const PendingUpload& now);
/** Another attachment starts while this one shows failed: the row goes (its staged copy is left as it is). */
PendingUploadStep OnOtherAttachmentStarted(const PendingUpload& now);
/** The app locked: the thumbnail is deleted and forgotten, the rest stays (the row shows the grey placeholder). */
PendingUploadStep OnAppLocked(const PendingUpload& now);

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

/**
 * The items being sent, in order, each in its chat. Pure: every change returns the thumbnails it lets go, for the
 * caller to delete. An item whose event makes it go (sent, cancelled, given up) leaves the list.
 */
class PendingUploadQueue {
public:
  const std::vector<PendingUpload>& Items() const { return items_; }
  bool Empty() const { return items_.empty(); }
  /** The item with this id; null when there is none. */
  const PendingUpload* Find(const std::string& id) const;
  /** The item shown in `thread_id` that is uploading or cancelling (the one in flight there); null when none. */
  const PendingUpload* InFlightIn(const std::string& thread_id) const;
  /** Some item shows in `thread_id`. */
  bool ShowsIn(const std::string& thread_id) const;
  void Add(PendingUpload item);
  /** `event` applied to item `id` (nothing when there is none); the item leaves when it no longer shows. */
  template <typename Event>
  std::vector<std::string> Apply(const std::string& id, Event&& event) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (it->row_id != id) {
        continue;
      }
      PendingUploadStep step = event(*it);
      std::vector<std::string> files;
      if (!step.delete_file.empty()) {
        files.push_back(std::move(step.delete_file));
      }
      if (step.next.Shows()) {
        *it = std::move(step.next);
      } else {
        items_.erase(it);
      }
      return files;
    }
    return {};
  }
  /** `event` applied to every item (another attachment starting, the lock). */
  template <typename Event>
  std::vector<std::string> ApplyAll(Event&& event) {
    std::vector<std::string> files;
    std::vector<std::string> ids;
    for (const PendingUpload& item : items_) {
      ids.push_back(item.row_id);
    }
    for (const std::string& id : ids) {
      for (std::string& file : Apply(id, event)) {
        files.push_back(std::move(file));
      }
    }
    return files;
  }
  /** Every item goes (profile reset, shutdown); their thumbnails are handed back. */
  std::vector<std::string> Clear();

private:
  std::vector<PendingUpload> items_;
};

} // namespace pbr
