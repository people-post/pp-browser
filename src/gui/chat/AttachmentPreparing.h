#pragma once

namespace pbr {

/** How the copy of a picked / captured file before its upload ended. Every one of them ends the "preparing" state. */
enum class AttachmentPrepareOutcome {
  Delivered,    // the file's path arrived; the upload (or the picture choice) takes over
  NoFile,       // the copy produced nothing (failed, iCloud video not available, ...)
  Cancelled,    // the person backed out
  Unavailable,  // the camera / picker could not deliver
  Unprocessable // the file came back but could not be made safe to send
};

/**
 * The gap on a phone between the person accepting a video (camera "Use Video", library pick) and the file path
 * arriving: the system copies it out first. While it lasts the composer shows "Sending…" and takes no second
 * attachment. Pure state: the chat controller starts it from the platform's `on_preparing` and ends it on every
 * way out, so it can never stay on.
 */
class AttachmentPreparing {
public:
  /** False (and nothing changes) when a preparation is already on: the second entry is refused. */
  bool Begin() {
    if (busy_) {
      return false;
    }
    busy_ = true;
    return true;
  }
  /** Every exit ends it, whatever the outcome. */
  void End(AttachmentPrepareOutcome /*outcome*/) { busy_ = false; }
  bool Busy() const { return busy_; }

private:
  bool busy_ = false;
};

} // namespace pbr
