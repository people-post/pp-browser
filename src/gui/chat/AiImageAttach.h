#pragma once

#include "feature/ai/AgentSession.h"
#include "foundation/platform/AiImagePrep.h"

#include <string>
#include <utility>

namespace pbr {

/** Inputs of the composer's attach-icon rule. */
struct AttachButtonInput {
  bool peer_thread_attach = false; // the peer-chat rule (call actions on, not an AI thread)
  bool ai_composer = false;        // an AI thread, or the home composer (its first send opens one)
  bool brief_preset = false;       // only the brief stream carries an image
  bool ai_usable = false;          // messaging ready and a brief key available
};

/** Peer threads keep their own rule; an AI composer offers the icon only when it can send an image. */
inline bool ShowAttachButton(const AttachButtonInput& in) {
  return in.ai_composer ? (in.brief_preset && in.ai_usable) : in.peer_thread_attach;
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

/** Longest edge of an image shown in a bubble, in dp. */
constexpr int kAiImageBubbleMaxDp = 240;

/** `width` x `height` scaled down (never up) to fit a kAiImageBubbleMaxDp square, keeping the aspect ratio. */
inline std::pair<int, int> AiImageBubbleSize(const int width, const int height) {
  const int longest = width > height ? width : height;
  if (width <= 0 || height <= 0 || longest <= kAiImageBubbleMaxDp) {
    return {width, height};
  }
  const auto scale = [longest](const int side) {
    const int scaled = side * kAiImageBubbleMaxDp / longest;
    return scaled > 0 ? scaled : 1;
  };
  return {scale(width), scale(height)};
}

/**
 * Rewrites the bubble of a stored image question ("[Image] <question>", see kAiImageTurnMarker).
 * `image_src` non-empty: the marker becomes a thumbnail (`width` x `height` are the image's pixels) above the question. Empty (the session file is
 * gone, e.g. after a restart): the marker is shown as `marker_label`. Anything else is returned as is.
 */
inline std::string DecorateAiImageBubble(std::string rml, const std::string& image_src, const int width,
                                         const int height, const std::string& marker_label) {
  const std::string anchor = std::string("<p class=\"bubble-text\">") + kAiImageTurnMarker;
  const size_t at = rml.find(anchor);
  if (at == std::string::npos) {
    return rml;
  }
  const std::string head = "<p class=\"bubble-text\">";
  // Explicit size: with only max-width/max-height the image box kept its full width and ran out of the bubble.
  const auto [shown_w, shown_h] = AiImageBubbleSize(width, height);
  const std::string size = shown_w > 0 && shown_h > 0 ? " style=\"width: " + std::to_string(shown_w) +
                                                            "dp; height: " + std::to_string(shown_h) + "dp;\""
                                                      : std::string();
  const std::string replacement = image_src.empty()
                                      ? head + marker_label + " "
                                      : "<img class=\"chat-ai-image\" src=\"" + image_src + "\"" + size + "/>" + head;
  rml.replace(at, anchor.size(), replacement);
  return rml;
}

} // namespace pbr
