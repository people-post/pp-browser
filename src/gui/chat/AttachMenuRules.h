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

/** What the composer locks: the text input (with emoji and send), and the attach entries (plus, camera, microphone). */
struct ComposerLocks {
  bool input = false;
  bool attach = false;
};

/**
 * An attachment in flight or failed in the open chat (`attachment_here`) locks the whole composer while its card above
 * the input says why. A bubble in the transcript instead (`bubble_in_transcript`, no card): text stays typeable and
 * sendable there. The attach entries (plus, camera, microphone) are off only while an attachment is preparing or
 * uploading (`attachment_busy`, in any chat: one at a time); beside a failed bubble alone they work, and a new
 * attachment makes that bubble give way (DEV/PLAN-attachment-bubbles-multi-pick.md goal 4).
 */
inline ComposerLocks ComposerLocksOf(const bool compose_disabled, const bool attachment_here,
                                     const bool bubble_in_transcript, const bool attachment_busy,
                                     const bool image_preparing, const bool voice_recording,
                                     const bool editing_message = false) {
  ComposerLocks locks;
  locks.input = compose_disabled || (attachment_here && !bubble_in_transcript) || image_preparing || voice_recording;
  // Editing a sent message changes its words only: no picture, file or voice message meanwhile.
  locks.attach = locks.input || attachment_busy || editing_message;
  return locks;
}

} // namespace pbr
