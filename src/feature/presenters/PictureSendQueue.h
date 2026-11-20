#pragma once

#include "common/Error.h"
#include "lib/messaging/AttachmentLimits.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Pictures picked together go out one at a time, with the line typed beside them sent after the last one as an
 * ordinary text message (DEV/PLAN-picture-send-preview.md): no new message format, a picture is a picture and the
 * text is text. Pure: no GUI, no clock and no thread of its own (the caller passes the time through a port); the chat
 * screen wires the ports and does what they ask.
 */

/** How one attachment send ended, as the picture queue needs to know it (StartAttachmentUpload's `on_finished`). */
enum class AttachmentSendOutcome {
  Sent,
  /** Did not go out; its encrypted copy is staged, so its bubble stays with Retry. */
  FailedStaged,
  /** Did not go out and nothing was kept (refused, could not be read, lost before staging). */
  FailedNotStaged,
  /** Refused by this device's send pace before anything was staged: worth sending again a little later. */
  RateLimited,
  /** Stopped by the person (or let go with its pick) before it went out. */
  Cancelled,
};

/**
 * The outcome from what the send's callback knows: `sent` wins (a cancel that came too late changes nothing); then a
 * cancel; then a staged copy (a server refusal after staging, 429 included, keeps its Retry); then the device's own
 * pace refusal (`rate_limited`, which only ever comes before staging); anything else did not go and kept nothing.
 */
AttachmentSendOutcome ClassifyUploadOutcome(bool sent, bool cancelled, bool staged, bool rate_limited);

/** The error is the attachment pace refusal (Err::Blob::RateLimited). */
bool IsAttachmentRateLimited(const ::pp::Error& error);

/**
 * A send's end reported once. Every copy shares one state: the first Fire calls `done`, later ones do nothing. When
 * the last copy goes without any Fire (a callback the pipeline dropped), `done` gets `if_dropped`, so nobody waits for
 * an end that never comes. A default-made one (no `done`) does nothing. Fire and the last copy's end may happen on any
 * thread; `done` runs on that thread.
 */
class AttachmentSendOnce {
public:
  AttachmentSendOnce() = default;
  explicit AttachmentSendOnce(std::function<void(AttachmentSendOutcome)> done,
                              AttachmentSendOutcome if_dropped = AttachmentSendOutcome::FailedNotStaged);
  void Fire(AttachmentSendOutcome outcome) const;
  bool Fired() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

enum class PictureSendKind { Picture, Video };

/** Where an item is. Every state after Sending is final; RateLimited sends it back to Waiting. */
enum class PictureItemState {
  Waiting,
  Sending,
  Sent,
  /** Failed with its bubble (and Retry) left in the chat. */
  FailedStaged,
  /** Failed and nothing kept. */
  FailedNotStaged,
  /** Over the limit for how it would be sent: skipped, never offered to the sender. */
  TooLarge,
  /** Not sent: the person cancelled it or the queue stopped before it. */
  Cancelled,
};

struct PictureSendItem {
  /** Stable while the item lives (indices move when one is removed). */
  uint64_t id = 0;
  /** The picked copy (tmp/pp-picks/…): the queue owns it and releases it once, also after a send. */
  std::string path;
  PictureSendKind kind = PictureSendKind::Picture;
  uint64_t bytes = 0;
  /** Width × height from the file's head; 0 when it could not be read. */
  uint64_t pixels = 0;
  /** The preview page's own files for it (deleted with the item, and when sending starts). */
  std::string preview_path;
  std::string thumb_path;
  /** It could not be sent the way it would go now (see PictureOverLimit): not counted, not sent. */
  bool too_large = false;
  PictureItemState state = PictureItemState::Waiting;
};

/**
 * The item cannot be sent the way it would go: a video over `max_video_bytes`; a picture sent as it is (`original`)
 * over the original-picture byte limit; a picture to be shrunk with more pixels than may be decoded.
 */
bool PictureOverLimit(const PictureSendItem& item, bool original, const AttachmentLimits& limits);

/** How the queue ended. */
enum class PictureSendExit {
  /** Every item had its turn (some may have failed). */
  Finished,
  /** Cancel() stopped the rest while sending (no control calls it today; see Cancel). */
  Cancelled,
  /** The device was offline: the rest was given up. */
  Offline,
  /** The preview page was left (back, a swipe, the layer closed by anyone): nothing was sent. */
  Back,
  ChatDeleted,
  Locked,
  SignedOut,
};

/** Where the typed line went. */
enum class PictureTextFate {
  /** There was none. */
  None,
  /** Sent after the last picture. */
  Sent,
  /** Back into the composer of the chat the pictures were picked in, after what it had. */
  PutBack,
  /** Gone with the chat or the session. */
  Dropped,
};

struct PictureSendReport {
  PictureSendExit exit = PictureSendExit::Finished;
  PictureTextFate text = PictureTextFate::None;
  int sent = 0;
  int failed_staged = 0;
  int failed_not_staged = 0;
  int too_large = 0;
  int cancelled = 0;
  /** Still being sent when the queue ended (lock, sign-out, chat deleted, cancel): its end comes later. */
  int in_flight = 0;
  /** The pictures that did not go out ("N not sent"); the one still in flight is not counted. */
  int NotSent() const { return failed_staged + failed_not_staged + too_large + cancelled; }
};

/** "Sending 3 of 9": `number` is the item being sent (or waited for), `of` the items that will be sent. */
struct PictureSendProgress {
  size_t number = 0;
  size_t of = 0;
  /** Waiting out the send pace before the item `number` is tried again. */
  bool paced = false;
};

/** What the queue needs from the app. Every port is called on the caller's thread, from inside a queue call. */
struct PictureSendPorts {
  /**
   * Starts sending one item (`original`: a picture as it is, else shrunk; a video is always as it is). Exactly one
   * OnItemFinished(item.id, …) must follow, now or later. The file is not the sender's to release.
   */
  std::function<void(const PictureSendItem& item, bool original)> send_item;
  /** The typed line as an ordinary text message in `thread_id`; false when it did not go (it is put back then). */
  std::function<bool(const std::string& thread_id, const std::string& text)> send_text;
  /** Unsent text back into `thread_id`'s composer, after a new line when it already has text. */
  std::function<void(const std::string& thread_id, const std::string& text)> put_back_text;
  /** Deletes a picked copy that is no longer needed (once per item). */
  std::function<void(const std::string& path)> release;
  /** Deletes a preview page file of an item. */
  std::function<void(const std::string& path)> remove_file;
  /** A steady clock in milliseconds. */
  std::function<int64_t()> now_ms;
  /** Asks for a Tick() in about `delay_ms` (the send pace wait). */
  std::function<void(int64_t delay_ms)> schedule_tick;
  /** The device knows it is offline now. */
  std::function<bool()> offline;
  /** The queue ended (once). Must not destroy the queue from inside the call. */
  std::function<void(const PictureSendReport& report)> done;
};

/**
 * The pictures of one pick, in the order they were chosen, for the chat they were picked in. Before Start they can be
 * added (at most kMaxItems), removed, given preview files, and sent as they are or shrunk ("原图"). Start sends them
 * one at a time through `send_item` — the next only after the previous one's OnItemFinished — and the typed line
 * after the last, whatever failed. A pace refusal (RateLimited) is not a failure: the same item goes again after
 * kRateLimitRetryMs, as often as it takes (an offline device, a lock or Cancel() stops it). The queue holds the attachment slot from Start until it
 * ends (Active()), gaps between items and pace waits included, so no other attachment slips in between.
 */
class PictureSendQueue {
public:
  static constexpr size_t kMaxItems = 9;
  static constexpr int64_t kRateLimitRetryMs = 15000;

  PictureSendQueue(std::string thread_id, AttachmentLimits limits, PictureSendPorts ports);

  /** The chat the pictures were picked in: they and the text go there, and the text goes back there. */
  const std::string& ThreadId() const { return thread_id_; }
  const std::vector<PictureSendItem>& Items() const { return items_; }

  /**
   * A picked copy arrived. Its id; none when it cannot join (the queue is full or ended): its file is released then.
   * After Start it joins the end of the line (more were still being copied when Send was tapped).
   */
  std::optional<uint64_t> Add(std::string path, PictureSendKind kind, uint64_t bytes, uint64_t pixels = 0);
  /** More copies are on their way: after Start the queue waits for them before it ends. */
  void SetArriving(bool arriving);
  /** Before Start only: the item goes, its copy is released and its preview files deleted. */
  bool Remove(uint64_t id);
  /** Before Start only: the item's preview files; false (the caller deletes them) when it is gone or sending began. */
  bool SetPreview(uint64_t id, std::string preview_path, std::string thumb_path);
  /** Before Start only: "原图" for the whole batch; the over-limit marks are decided again. */
  void SetOriginal(bool original);
  bool Original() const { return original_; }
  void SetText(std::string text);
  const std::string& Text() const { return text_; }
  /** The bytes of every item picked (the size shown beside "原图"). */
  uint64_t SelectedBytes() const;
  /** The items that will be sent (not over the limit). */
  size_t SendableCount() const;

  /**
   * Sending begins: preview files are deleted, over-limit items skipped (released), the first item offered. False
   * when it began already, ended, or has nothing to send and nothing on its way.
   */
  bool Start();
  /** The end of item `id`'s send. Anything but the item in flight (a second report, a stale one) is ignored. */
  void OnItemFinished(uint64_t id, AttachmentSendOutcome outcome);
  /** The time asked for by `schedule_tick` may have come: a paced item goes again when it has. */
  void Tick();

  /**
   * Stops the whole batch: the item in flight finishes, the rest is not sent, the text goes back. NOT wired to any
   * control today: the cancel disc on an uploading bubble cancels only that item (the batch goes on). Kept, with
   * its tests, for a future "stop all" control.
   */
  void Cancel();
  /** The preview page was left before Start: nothing is sent, every copy released, the text goes back. */
  void Back();
  /** The chat was deleted: the rest released, the text dropped. */
  void ChatDeleted();
  /** The vault locked: the rest released, the text dropped. */
  void Locked();
  /** Signed out (or the profile reset / teardown): the rest released, the text dropped. */
  void SignedOut();

  /** Started and not ended: the attachment slot is the queue's (gaps and pace waits included). */
  bool Active() const { return started_ && !ended_; }
  bool Started() const { return started_; }
  bool Ended() const { return ended_; }
  /** An item's send has not reported yet (also after the queue ended: its end is still taken). */
  bool InFlight() const { return in_flight_ != 0; }
  /** Nothing more will happen here: ended, nothing in flight. The owner may let it go. */
  bool Settled() const { return ended_ && in_flight_ == 0; }
  PictureSendProgress Progress() const;
  /** The limits this batch is checked against (the send path's current ones when the batch began). */
  const AttachmentLimits& Limits() const { return limits_; }

private:
  PictureSendItem* Find(uint64_t id);
  void RemovePreviewFiles(PictureSendItem& item);
  void ReleaseItem(PictureSendItem& item, PictureItemState final_state);
  void Pump();
  void PumpOnce();
  void End(PictureSendExit exit);

  std::string thread_id_;
  AttachmentLimits limits_;
  PictureSendPorts ports_;
  std::vector<PictureSendItem> items_;
  uint64_t next_id_ = 0;
  bool original_ = false;
  std::string text_;
  bool arriving_ = false;
  bool started_ = false;
  bool ended_ = false;
  uint64_t in_flight_ = 0;
  std::optional<int64_t> retry_at_ms_;
  bool pumping_ = false;
  bool pump_again_ = false;
};

} // namespace pbr
