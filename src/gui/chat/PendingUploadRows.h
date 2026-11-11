#pragma once

#include "feature/conversations/GroupMentions.h"
#include "feature/conversations/InboxController.h"
#include "feature/presenters/PendingUpload.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "lib/ui/ChatWidgetTypes.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ui {
class Element;
} // namespace ui

namespace pbr {

/**
 * The transcript's side of the attachments being sent (DEV/PLAN-picture-upload-bubble.md,
 * DEV/PLAN-attachment-bubbles-multi-pick.md): it holds their queue (PendingUploadQueue, whose pure events decide
 * everything), draws their rows with the transcript's own row builder, keeps those rows at the end of the rows shown,
 * deletes the thumbnails it lets go, and writes an item's upload percent into its mounted bubble without drawing the
 * row again. ChatController feeds it the events and redraws the transcript when Apply says a row changed; the rows
 * are never stored. One attachment is sent at a time, so the queue holds at most one item today; Upload() / Apply(step)
 * act on that current item.
 */
class PendingUploadRows {
public:
  using RemoveFile = std::function<void(const std::string& path)>;
  using Event = std::function<PendingUploadStep(const PendingUpload&)>;
  explicit PendingUploadRows(RemoveFile remove_file = {});

  const PendingUploadQueue& Queue() const { return queue_; }
  /** The current item (the first in the queue); an empty one (state None) when there is none. */
  const PendingUpload& Upload() const;
  /**
   * Takes an event's result for the current item (a result for an id not in the queue that shows is added): keeps
   * its value and deletes its file. True when the row as drawn changes. A thumbnail taken away (the lock) is taken out
   * of the kept row too, so putting it back never points at the deleted file.
   */
  bool Apply(PendingUploadStep step);
  /** `event` applied to item `id` (an event carrying the item's id); false and nothing done when there is none. */
  bool Apply(const std::string& id, const Event& event);
  /** Every bubble goes without a send ending (profile reset, shutdown): thumbnails deleted, the queue cleared. */
  void Drop();
  /**
   * The kept rows' markup copied into the rows of them at the end of `messages` (after Apply changed them without a
   * redraw). True when one of them was there and changed.
   */
  bool RefreshTail(std::vector<MessageDisplayRow>& messages) const;

  /**
   * `entries` (oldest first) drawn as RenderTranscript draws them, with the items of `thread_id` after them as our
   * messages sent now — when the rows end at the chat's newest message (`at_tip`) — so they get the date strip and run
   * as the real rows will, and the row before them loses its tail as it will then. Those rows are kept aside (Rows());
   * only the real rows are returned, so paging and counting never see them.
   */
  std::vector<MessageDisplayRow> Render(std::vector<TranscriptRow> entries, TranscriptFacts facts,
                                        const std::optional<MentionRoster>& mentions, const std::string& thread_id,
                                        bool at_tip);
  /** The rows Render last drew, in order (none when no item was drawn there). */
  const std::vector<MessageDisplayRow>& Rows() const { return rows_; }
  /** The first of them (tests). */
  std::optional<MessageDisplayRow> Row() const;
  /**
   * Puts those rows at the end of `messages` when they belong there (their chat is `thread_id`, the rows reach the
   * tip) and are not all there yet. True when the end changed.
   */
  bool KeepAtTail(std::vector<MessageDisplayRow>& messages, const std::string& thread_id, bool at_tip) const;
  /** The last real row's id and the number of real rows: pending rows at the end are not counted. */
  static std::pair<std::string, size_t> RealTail(const std::vector<MessageDisplayRow>& messages);

  /**
   * Upload progress of the item uploading now: stored; its id when the percent shown is due to be drawn again
   * (PendingProgressDue), none otherwise.
   */
  std::optional<std::string> Progress(int percent);
  /**
   * Writes `percent` into item `id`'s bubble mounted under `root`: the ring's decorator (when the ring is in use) and
   * the line under the disc. False when it is not mounted; the row's next drawing shows the stored percent then.
   */
  static bool DrawProgress(ui::Element* root, const std::string& id, int percent);
  /** Takes item `id`'s thumbnail out of its bubble mounted under `root` (the app locked: its file is gone). */
  static void DropThumbnail(ui::Element* root, const std::string& id);

private:
  bool AfterChange(const std::string& id, const std::optional<PendingUpload>& before, std::vector<std::string> files);

  RemoveFile remove_file_;
  PendingUploadQueue queue_;
  std::vector<MessageDisplayRow> rows_;
  std::map<std::string, int> drawn_percent_;
};

} // namespace pbr
