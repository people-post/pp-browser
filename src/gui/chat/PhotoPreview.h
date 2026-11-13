#pragma once

#include "common/chat/MessagingLimits.h"

#include <cstdint>
#include <string>
#include <utility>

namespace pbr {

/** Mount element (ShellLayoutRml) that holds the picture preview; MountOverlayBody fills / clears it. */
inline constexpr const char* kPhotoPreviewMountId = "shell-photo-mount";

/**
 * What shows the moment a picture is tapped, until the system viewer is up: a spinner on the dimmed screen. No picture
 * (B203): the bubble only holds the small preview, blurred full screen; the viewer gets the full picture. A tap
 * anywhere closes it (`photo_preview_close`).
 */
inline std::string PhotoPreviewRml() {
  return "<div class=\"photo-preview\" data-model=\"window\" data-event-click=\"photo_preview_close()\">"
         "<div class=\"photo-preview-spinner\"></div></div>";
}

/** The "Opening..." toast is only for a non-picture above the direct size (decrypting it takes a moment). */
inline bool OpenShowsOpeningToast(const bool picture_preview, const uint64_t byte_length) {
  return !picture_preview && byte_length > kMaxDirectAttachmentBytes;
}

/**
 * Which picture preview is up. Begin: a tap (true: mount it). Finish: the open's result arrived, success or
 * failure (true: it was up for this message, clear it).
 */
class PhotoPreviewTracker {
public:
  bool Begin(std::string message_id) {
    message_id_ = std::move(message_id);
    return !message_id_.empty();
  }
  bool Finish(const std::string& message_id) {
    if (message_id_.empty() || message_id_ != message_id) {
      return false;
    }
    message_id_.clear();
    return true;
  }
  bool Up() const { return !message_id_.empty(); }

private:
  std::string message_id_;
};

} // namespace pbr
