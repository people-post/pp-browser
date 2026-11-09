#pragma once

#include "feature/ai/AgentSession.h"
#include "lib/messaging/AiImageRef.h"
#include "foundation/platform/AiImagePrep.h"
#include "gui/chat/TranscriptRml.h"

#include <string>

namespace pbr {

/** Inputs of the composer's attach-icon rule. */
struct AttachButtonInput {
  bool peer_thread_attach = false; // the peer-chat rule (call actions on, not an AI thread)
  bool ai_composer = false;        // an AI thread, or the home composer (its first send opens one)
  bool brief_preset = false;       // only the brief stream carries an image
  bool ai_usable = false;          // messaging ready and a brief key available
};

/** Peer threads keep their own rule. An AI composer shows the key whenever the preset can take an image at all
 *  (brief), whether or not the AI is reachable right now: a key that appears late or vanishes read as a bug (Kenneth
 *  2026-11-09); when the AI is not ready, the tap says so instead. */
inline bool ShowAttachButton(const AttachButtonInput& in) {
  return in.ai_composer ? in.brief_preset : in.peer_thread_attach;
}

/** Locale key for a failed PrepareAiImageFromFile; the cause decides the wording, never a generic hint. */
inline const char* AiImagePrepErrorKey(const AiImagePrepError error) {
  switch (error) {
  case AiImagePrepError::NotAnImage:
    return "chat.image.not_supported";
  case AiImagePrepError::TooLarge:
    return "chat.image.too_large";
  case AiImagePrepError::Failed:
    break;
  }
  return "chat.image.read_failed";
}

/** Locale key for AgentEvent::error_kind of an image turn; nullptr when the server's own wording stays. */
inline const char* AiImageErrorKindKey(const std::string& error_kind) {
  if (error_kind == "image_too_large") {
    return "chat.image.too_large";
  }
  if (error_kind == "image_unsupported") {
    return "chat.image.not_supported";
  }
  return nullptr;
}

/** The picture bubble's frame round the picture (`.bubble-user--picture` padding, components.rcss). */
inline constexpr int kAiPictureFrameDp = 3;

/**
 * Rewrites the bubble of a stored image question ("[Image] <question>", see kAiImageTurnMarker).
 * `image_src` non-empty: the bubble becomes a picture bubble like a chat's (B181, FitPictureInBubble): the picture
 * fitted inside 200 x 240dp (`width` x `height` are its pixels), in a clip box with rounded corners (the engine does
 * not round an <img>), the question under it; the bubble is exactly as wide as the picture plus its 3dp frame, so the
 * question wraps inside it and nothing sticks out. Empty (no stored image, or the profile is locked): the marker is
 * shown as `marker_label`. Anything else is returned as is.
 */
inline std::string DecorateAiImageBubble(std::string rml, const std::string& image_src, const int width,
                                         const int height, const std::string& marker_label) {
  const std::string anchor = std::string("<p class=\"bubble-text\">") + kAiImageTurnMarker;
  const size_t at = rml.find(anchor);
  if (at == std::string::npos) {
    return rml;
  }
  const std::string head = "<p class=\"bubble-text\">";
  if (image_src.empty()) {
    rml.replace(at, anchor.size(), head + marker_label + " ");
    return rml;
  }
  const PictureBubbleSize fit = FitPictureInBubble(width, height);
  const std::string size = fit.width_dp > 0 ? " style=\"width: " + std::to_string(fit.width_dp) + "dp; height: " +
                                                  std::to_string(fit.height_dp) + "dp;\""
                                            : std::string();
  const std::string img_size =
      fit.width_dp > 0 ? " width=\"" + std::to_string(fit.width_dp) + "\" height=\"" + std::to_string(fit.image_height_dp) +
                             "\"" + (fit.image_offset_dp > 0 ? " style=\"margin-top: -" + std::to_string(fit.image_offset_dp) + "dp;\"" : "")
                       : std::string();
  rml.replace(at, anchor.size(),
              "<div class=\"chat-ai-image-clip\"" + size + "><img class=\"chat-ai-image\" src=\"" + image_src + "\"" +
                  img_size + "/></div>" + head);
  // The user bubble (its class may carry more names, e.g. the tail's host): a picture bubble, as wide as the picture.
  const std::string bubble = "<div class=\"bubble bubble-user";
  const size_t open = rml.rfind(bubble, at);
  if (open == std::string::npos) {
    return rml;
  }
  const size_t class_end = rml.find('"', open + std::string("<div class=\"").size());
  if (class_end == std::string::npos || class_end > at) {
    return rml;
  }
  std::string extra = " bubble-user--picture\"";
  if (fit.width_dp > 0) {
    extra += " style=\"width: " + std::to_string(fit.width_dp + 2 * kAiPictureFrameDp) + "dp;\"";
  }
  rml.replace(class_end, 1, extra);
  return rml;
}

} // namespace pbr
