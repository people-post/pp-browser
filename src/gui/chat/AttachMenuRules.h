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
 * the input says why. A picture being sent shows as a bubble in the transcript instead (`picture_in_transcript`, no
 * card): text stays typeable and sendable there; only another attachment waits (one at a time per chat).
 */
inline ComposerLocks ComposerLocksOf(const bool compose_disabled, const bool attachment_here,
                                     const bool picture_in_transcript, const bool image_preparing,
                                     const bool voice_recording) {
  ComposerLocks locks;
  locks.input = compose_disabled || (attachment_here && !picture_in_transcript) || image_preparing || voice_recording;
  locks.attach = locks.input || attachment_here;
  return locks;
}

} // namespace pbr
