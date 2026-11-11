#pragma once

#include "feature/conversations/GroupMentions.h"
#include "feature/conversations/InboxController.h"
#include "feature/presenters/PendingPictureUpload.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "lib/ui/ChatWidgetTypes.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ui {
class Element;
} // namespace ui

namespace pbr {

/**
 * The transcript's side of the picture being sent (DEV/PLAN-picture-upload-bubble.md): it holds the upload's state
 * (PendingPictureUpload, whose pure events decide everything), draws its row with the transcript's own row builder,
 * keeps that row at the end of the rows shown, deletes the thumbnails it lets go, and writes the upload percent into
 * the mounted bubble without drawing the row again. ChatController feeds it the events and redraws the transcript
 * when Apply says the row changed; the row is never stored.
 */
class PendingPictureRows {
public:
  using RemoveFile = std::function<void(const std::string& path)>;
  explicit PendingPictureRows(RemoveFile remove_file = {});

  const PendingPictureUpload& Upload() const { return upload_; }
  /**
   * Takes an event's result: keeps its value and deletes its file. True when the row as drawn changes. A thumbnail
   * taken away (the lock) is taken out of the kept row too, so putting it back never points at the deleted file.
   */
  bool Apply(PendingPictureStep step);
  /** The bubble goes without a send ending (profile reset, shutdown): its thumbnail is deleted, the state cleared. */
  void Drop();
  /**
   * The kept row's markup copied into the row of it at the end of `messages` (after Apply changed it without a
   * redraw). True when that row was there and changed.
   */
  bool RefreshTail(std::vector<MessageDisplayRow>& messages) const;

  /**
   * `entries` (oldest first) drawn as RenderTranscript draws them, with the picture being sent after them as our
   * message sent now — when it belongs to `thread_id` and the rows end at the chat's newest message (`at_tip`) — so it
   * gets the date strip and run as the real row will, and the row before it loses its tail as it will then. That row
   * is kept aside (Row()); only the real rows are returned, so paging and counting never see it.
   */
  std::vector<MessageDisplayRow> Render(std::vector<TranscriptRow> entries, TranscriptFacts facts,
                                        const std::optional<MentionRoster>& mentions, const std::string& thread_id,
                                        bool at_tip);
  /** The row Render last drew (none when the picture was not drawn there). */
  const std::optional<MessageDisplayRow>& Row() const { return row_; }
  /**
   * Puts that row at the end of `messages` when it belongs there (its chat is `thread_id`, the rows reach the tip) and
   * is not there yet. True when it was added.
   */
  bool KeepAtTail(std::vector<MessageDisplayRow>& messages, const std::string& thread_id, bool at_tip) const;
  /** The last real row's id and the number of real rows: a pending row at the end is not counted. */
  static std::pair<std::string, size_t> RealTail(const std::vector<MessageDisplayRow>& messages);

  /** Upload progress: stored; true when the percent shown is due to be drawn again (PendingProgressDue). */
  bool Progress(int percent);
  /**
   * Writes `percent` into the bubble mounted under `root`: the ring's decorator (when the ring is in use) and the line
   * under the disc. False when the bubble is not mounted; the row's next drawing shows the stored percent then.
   */
  static bool DrawProgress(ui::Element* root, int percent);
  /** Takes the thumbnail out of the bubble mounted under `root` (the app locked: its file is gone). */
  static void DropThumbnail(ui::Element* root);

private:
  RemoveFile remove_file_;
  PendingPictureUpload upload_;
  std::optional<MessageDisplayRow> row_;
  int drawn_percent_ = 0;
};

} // namespace pbr
