#pragma once

namespace pbr {

/**
 * The camera entry in the composer pill. Phones only; not while a call has the camera or microphone, not in an AI
 * composer (its paperclip has its own picture flow), and not while an attachment is uploading or waits for Retry /
 * Cancel (the same gate as the paperclip).
 */
inline bool ShowCaptureEntry(const bool is_mobile, const bool call_live, const bool in_ai_context,
                             const bool attachment_busy) {
  return is_mobile && !call_live && !in_ai_context && !attachment_busy;
}

} // namespace pbr
