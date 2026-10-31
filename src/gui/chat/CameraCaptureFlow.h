#pragma once

#include "foundation/platform/CameraCapture.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentLimits.h"
#include "lib/ui/ContextMenuHost.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pbr {

/** What the person is told when a capture did not lead to a send. Cancelled says nothing. */
enum class CaptureNotice { CameraUnavailable, VideoUnprocessable, VideoTooLarge };

/** What the capture result needs from the chat; ChatController fills it, a test fills it with recorders. */
struct CaptureFlowPorts {
  /** Delete a captured file (ChatController::release_pick_). Every path handed in here is released exactly once by someone. */
  std::function<void(const std::string& path)> release;
  /** A picture: ask "Send / Send original / Cancel" (which then owns releasing it). */
  std::function<void(std::string path)> ask_picture;
  /** A video: send it as it is (StartAttachmentUpload owns releasing it from here on). */
  std::function<void(std::string path)> send_video;
  std::function<void(CaptureNotice notice)> say;
  std::function<uint64_t(const std::string& path)> file_size;
  std::function<AttachmentLimits()> limits;
};

/**
 * The result of the system camera, on the UI thread. A picture goes to the existing send choice; a video is checked
 * against the limit again (the camera does not always honour the size it was given) and sent, or released when too
 * large. Cancelled is silent. Unavailable / Unprocessable tell the person why nothing was sent.
 */
inline void HandleCaptureResult(const CaptureFlowPorts& ports, const CameraCaptureOutcome outcome,
                                std::vector<std::string> paths) {
  switch (outcome) {
  case CameraCaptureOutcome::Cancelled:
    break;
  case CameraCaptureOutcome::Unavailable:
    ports.say(CaptureNotice::CameraUnavailable);
    break;
  case CameraCaptureOutcome::Unprocessable:
    ports.say(CaptureNotice::VideoUnprocessable);
    break;
  case CameraCaptureOutcome::Picked: {
    if (paths.empty()) {
      ports.say(CaptureNotice::CameraUnavailable); // "picked" with no file: not silent
      break;
    }
    std::string path = std::move(paths.front());
    if (AttachmentMimeFromFilename(path).rfind("video/", 0) != 0) {
      ports.ask_picture(std::move(path));
      break;
    }
    if (!VideoWithinLimit(ports.file_size(path), ports.limits())) {
      ports.release(path);
      ports.say(CaptureNotice::VideoTooLarge);
      break;
    }
    ports.send_video(std::move(path));
    break;
  }
  }
}

/**
 * "Send / Send original / Cancel" for a picture. Whichever is chosen, the file is released exactly once: the two
 * sends hand it to `send` (StartAttachmentUpload releases it), Cancel releases it here.
 */
inline std::vector<ContextMenuAction> PictureChoiceActions(std::string path, std::function<void(const std::string&, bool original)> send,
                                                           std::function<void(const std::string&)> release,
                                                           const std::string& send_label, const std::string& original_label,
                                                           const std::string& cancel_label) {
  auto picture = std::make_shared<std::string>(std::move(path));
  std::vector<ContextMenuAction> actions;
  actions.push_back({"attach_send_compressed", send_label, nullptr, [picture, send]() { send(*picture, false); }});
  actions.push_back({"attach_send_original", original_label, nullptr, [picture, send]() { send(*picture, true); }});
  actions.push_back({"attach_cancel", cancel_label, nullptr, [picture, release]() { release(*picture); }});
  return actions;
}

} // namespace pbr
