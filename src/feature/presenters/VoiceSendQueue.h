#pragma once

#include "feature/presenters/PictureSendQueue.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Voice messages waiting for the attachment slot. The attachment pipeline sends one thing at a time, so a person who
 * records the next message while the last one uploads would be refused; instead each finished recording joins this
 * line (its bubble is in the transcript already) and goes out in the order it was recorded, one at a time, as soon as
 * the slot is free. A failure never holds the line: the next job goes whatever the last one's outcome, and a failed
 * bubble keeps its own Retry / ✕. Retry is a job too (it re-joins the end of the line). Pure: no GUI, no clock of its
 * own; the chat screen wires the ports and does what they ask.
 */

enum class VoiceJobKind {
  /** A finished recording at `path`, not sent yet. */
  Fresh,
  /** A failed bubble's staged copy, sent again. */
  Retry,
};

struct VoiceSendJob {
  VoiceJobKind kind = VoiceJobKind::Fresh;
  /** The job's bubble ("pending-upload-<n>"): the job's id, and what every event of the send carries. */
  std::string row_id;
  std::string thread_id;
  /** The recording's file (Fresh only); the queue deletes it when the job is cancelled or dropped unsent. */
  std::string path;
  /** The recording's size and length, for the timing line only. */
  uint64_t bytes = 0;
  int64_t duration_ms = 0;
};

/** How long one job waited for its turn and then took (from the start to its end), for the metrics line. */
struct VoiceSendTiming {
  int64_t wait_ms = 0;
  int64_t send_ms = 0;
};

/** The most voice messages of one chat waiting or being sent: at this number that chat's voice bar is off. */
inline constexpr size_t kVoiceSendMaxPerChat = 5;

struct VoiceSendPorts {
  /** No other attachment is being prepared or sent, and messaging is up: a job may start. */
  std::function<bool()> slot_free;
  /** Starts one job. Exactly one OnFinished(job.row_id, …) must follow, now or later. */
  std::function<void(const VoiceSendJob& job)> start;
  /** Deletes a recording that will not be sent. */
  std::function<void(const std::string& path)> remove_file;
  /** A steady clock in milliseconds. */
  std::function<int64_t()> now_ms;
  /** A job ended (any outcome). */
  std::function<void(const VoiceSendJob& job, AttachmentSendOutcome outcome, const VoiceSendTiming& timing)> report;
  /** A job joined or left the line (so a chat's count changed). Optional. */
  std::function<void()> changed;
};

class VoiceSendQueue {
public:
  explicit VoiceSendQueue(VoiceSendPorts ports);

  /** The job joins the end of the line. It starts at once when the slot is free and nothing is ahead of it. */
  void Enqueue(VoiceSendJob job);
  /** Starts the next job when none runs and the slot is free. Safe to call at any time, also from inside `start`. */
  void Pump();
  /** The end of the job `row_id`'s send; anything but the job in flight (a second report, a stale one) is ignored. */
  void OnFinished(const std::string& row_id, AttachmentSendOutcome outcome);
  /**
   * The person's cancel on a bubble whose job has not started: it leaves the line (a Fresh job's recording is deleted)
   * and is handed back for the caller to fix its bubble. Nothing when it is not waiting (in flight: the send's own
   * cancel applies).
   */
  std::optional<VoiceSendJob> CancelWaiting(const std::string& row_id);
  /**
   * Every job still waiting leaves (the vault locked, signed out, a profile reset), or only those of `thread_id`
   * (the chat was deleted). Recordings are deleted; the jobs are handed back. The one in flight is not touched.
   */
  std::vector<VoiceSendJob> DropWaiting(const std::optional<std::string>& thread_id = std::nullopt);

  bool IsWaiting(const std::string& row_id) const;
  /** Voice messages of `thread_id` waiting or being sent (a failed one has left the line). */
  size_t CountForThread(const std::string& thread_id) const;
  /** `thread_id` has kVoiceSendMaxPerChat voice messages out: its bar takes no new recording until one ends. */
  bool ChatFull(const std::string& thread_id) const { return CountForThread(thread_id) >= kVoiceSendMaxPerChat; }
  size_t WaitingCount() const { return waiting_.size(); }
  bool InFlight() const { return in_flight_.has_value(); }

private:
  struct Waiting {
    VoiceSendJob job;
    int64_t enqueued_ms = 0;
  };
  struct Running {
    VoiceSendJob job;
    int64_t wait_ms = 0;
    int64_t started_ms = 0;
  };
  int64_t Now() const { return ports_.now_ms ? ports_.now_ms() : 0; }
  void Discard(const VoiceSendJob& job);
  void Changed() const;

  VoiceSendPorts ports_;
  std::deque<Waiting> waiting_;
  std::optional<Running> in_flight_;
  bool pumping_ = false;
};

} // namespace pbr
