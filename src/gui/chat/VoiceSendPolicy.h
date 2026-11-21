#pragma once

#include "feature/presenters/PictureSendQueue.h"

#include <cstddef>

namespace pbr {

/** What happens to the temporary file of a voice recording once its send has finished (or not). */
enum class VoiceFileFate {
  Delete,       // sent, or staged (encrypted, sealed under the profile key): the staged copy is the retry path
  KeepForRetry, // nothing was staged (the send was refused or the file could not be prepared): the file is the only copy
};

/**
 * `staged_before` / `staged_after`: how many unfinished sends the chat held before the attempt and after it.
 * A failed send that left one more behind is staged; any other failure kept nothing but the recording itself.
 */
inline VoiceFileFate DecideVoiceFileFate(const bool sent, const size_t staged_before, const size_t staged_after) {
  if (sent || staged_after > staged_before) {
    return VoiceFileFate::Delete;
  }
  return VoiceFileFate::KeepForRetry;
}

/** What a voice send's end (not a cancel) does to its recording and its bubble. */
enum class VoiceSendSettle {
  QueueKeeps,             // refused by the send pace: the queue sends it again, file and bubble stay as they are
  DeleteFile,             // sent or staged: the file goes, the bubble is settled by the send's own outcome
  DeleteFileRemoveBubble, // reported as not staged, yet the chat holds one more staged send: the file goes and so
                          // does the bubble (it has no Retry of its own), never left "Uploading"
  KeepBehindRetry,        // nothing staged: the recording stays behind its own bubble's Retry
};

inline VoiceSendSettle DecideVoiceSendSettle(const AttachmentSendOutcome ended, const bool sent,
                                             const size_t staged_before, const size_t staged_after) {
  if (ended == AttachmentSendOutcome::RateLimited) {
    return VoiceSendSettle::QueueKeeps; // refused before staging: a count that grew is another send's
  }
  if (DecideVoiceFileFate(sent, staged_before, staged_after) == VoiceFileFate::Delete) {
    return ended == AttachmentSendOutcome::FailedNotStaged ? VoiceSendSettle::DeleteFileRemoveBubble
                                                           : VoiceSendSettle::DeleteFile;
  }
  return VoiceSendSettle::KeepBehindRetry;
}

/**
 * Opening a chat on a phone puts the composer back in the mode it was in last time on this device (keyboard or the
 * hold-to-talk bar). Never over text the person already wrote, while a message is being edited, where the chat cannot take
 * a voice message, or when the bar is up already.
 */
inline bool RestoreVoiceMode(const bool remembered_voice, const bool phone, const bool chat_can_voice,
                             const bool has_draft, const bool editing_message, const bool voice_mode_now) {
  return remembered_voice && phone && chat_can_voice && !has_draft && !editing_message && !voice_mode_now;
}

} // namespace pbr
