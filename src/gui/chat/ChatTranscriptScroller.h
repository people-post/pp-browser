#pragma once

#include "lib/ui/ChatWidgetTypes.h"
#include "feature/conversations/ConversationsFacade.h"

#include <ui/base/Types.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ui {
class Context;
class Element;
} // namespace ui

namespace pbr {

/** Follow-tail / scroll-up paging for the chat transcript (D031 + viewport policy). */
class ChatTranscriptScroller {
public:
  struct View {
    bool& show_jump_to_latest;
    ui::String& jump_to_latest_count;
    std::vector<MessageDisplayRow>& messages;
    bool& has_turns;
  };

  ChatTranscriptScroller(ui::Context*& context, View view, bool& messaging_ready);
  void BindConversationsFacade(ConversationsFacade* facade);

  void SetDirtyTurns(std::function<void()> dirty_turns) { dirty_turns_ = std::move(dirty_turns); }
  /**
   * Called when a page of older rows has been loaded into the window (measurement only): scrolled up into the local
   * history, or older history fetched from the peer (ExpandLoadedMinFromOlderPage).
   */
  void SetOnOlderLoaded(std::function<void()> on_loaded) { on_older_loaded_ = std::move(on_loaded); }
  /** Draws a thread's transcript window as bound rows (the chat controller renders and decorates). */
  using RowBuilder = std::function<std::vector<MessageDisplayRow>(
      const std::string& thread_id, std::optional<int64_t> oldest_inclusive, std::optional<int64_t> newest_inclusive)>;
  void SetRowBuilder(RowBuilder rows) { rows_ = std::move(rows); }
  /**
   * Marks rows after they are built (search inside the chat tints its hits). Runs on every path that fills the
   * transcript; empty = no marks.
   */
  using RowDecorator = std::function<void(std::vector<MessageDisplayRow>&)>;
  void SetRowDecorator(RowDecorator decorator) { decorator_ = std::move(decorator); }
  /** The thread's rows in [oldest, newest], built and decorated. */
  std::vector<MessageDisplayRow> Rows(const std::string& thread_id, std::optional<int64_t> oldest_inclusive,
                                      std::optional<int64_t> newest_inclusive) const;
  /** Marks the rows already shown again (search marks taken off, then the decorator), without building them. */
  void Redecorate();

  void Reset();
  void RequestScrollToLatest();
  void ApplyPolicy();
  void OnMessagesScroll();
  void OnJumpToLatest();
  /** Jumped: scrolling to it. NotFound: the store has no such message (deleted). Unavailable: no chat / store. */
  enum class JumpResult { Jumped, NotFound, Unavailable };
  /**
   * Show `message_id` of the open chat at the top instead of the latest (opening a chat at its oldest unread
   * mention, G015; a search hit): when it is outside the loaded window (older, or newer than a history window)
   * the window is drawn again from it, then it is scrolled into view once laid out. Call after the open's
   * display sync.
   */
  JumpResult JumpToMessage(const std::string& message_id);
  /** Where the window must go for a jump to `order`: unchanged when it is loaded, else drawn from it to the tip. */
  struct JumpWindow {
    bool refetch = false;
    std::optional<int64_t> loaded_min;
    std::optional<int64_t> loaded_max;
  };
  static JumpWindow WindowForJump(int64_t order, std::optional<int64_t> loaded_min, std::optional<int64_t> loaded_max);
  /** SyncLayout remounts DOM — re-arm follow-tail when still pinned, else put the reading position back. */
  void OnShellRemounted();

  /** No scroll to the bottom, jump or remount restore is still being settled (the rows on screen are where they stay). */
  bool Settled() const { return !pending_scroll_to_bottom_ && pending_jump_id_.empty() && !restore_scroll_top_; }

  const std::optional<int64_t>& LoadedMinDisplayOrder() const { return loaded_min_display_order_; }
  const std::optional<int64_t>& LoadedMaxDisplayOrder() const { return loaded_max_display_order_; }

  /**
   * Cap `messages` to kMaxMessagesDomWindow. When pinned, drop the oldest down to a batch below the cap
   * (kMessagesDomWindowTrimBatch) and raise loaded_min.
   * When unpinned (history), drop newest and set loaded_max. Returns true if trimmed.
   */
  static bool TrimDomWindow(std::vector<MessageDisplayRow>& messages, bool pinned_to_bottom,
                            std::optional<int64_t>& loaded_min, std::optional<int64_t>& loaded_max,
                            bool& has_more_local_history);

  /**
   * Scroll offset after the viewport changed height (on-screen keyboard shown / hidden): pinned stays on
   * the bottom; otherwise the content in view stays where it is, within the new range.
   */
  static float ScrollTopAfterViewportResize(float top, float old_client_height, float new_client_height,
                                            float scroll_height, bool pinned_to_bottom);

  /** Call at start of SyncDisplayFromThread; returns true if active thread id changed. */
  bool BeginDisplaySync(const std::string& thread_id);
  /** After rebuilding messages; updates watermarks and pin / jump-FAB. */
  void EndDisplaySync(bool thread_changed, const std::string& prev_tail_id, size_t prev_count);

  /** Capture scroll offset before peer older-history expands the window. */
  void CaptureScrollBeforePrependIfUnpinned();
  void ExpandLoadedMinFromOlderPage(const std::string& thread_id, int64_t before_display_order);

private:
  ui::Element* FindMessagesScrollElement() const;
  void UpdateJumpToLatestCount();
  void SetShowJumpToLatest(bool show);
  void ScrollMessagesToBottom();
  void MaybeLoadOlderLocalHistory();
  void LoadOlderLocalHistory();
  /** A pending JumpToMessage: scroll its row into view until the layout settles. True while it runs. */
  bool ApplyPendingJump(ui::Element* el);

  ui::Context*& context_;
  View view_;
  bool& messaging_ready_;
  std::function<void()> dirty_turns_;
  std::function<void()> on_older_loaded_;

  bool pinned_to_bottom_ = true;
  bool pending_scroll_to_bottom_ = false;
  int pending_scroll_settle_frames_ = 0;
  int pending_scroll_attempts_ = 0;
  float settle_scroll_height_ = -1.f;
  bool suppress_scroll_handler_ = false;
  bool loading_older_local_ = false;
  bool has_more_local_history_ = false;
  std::string scroll_thread_id_;
  std::optional<int64_t> loaded_min_display_order_;
  /** When set, transcript window excludes tip rows newer than this (after history trim). */
  std::optional<int64_t> loaded_max_display_order_;
  std::optional<float> pending_scroll_height_before_;
  std::optional<float> pending_scroll_top_before_;
  /** Where the person is reading (not pinned to the bottom), kept every frame so a remount can put it back. */
  std::optional<float> reading_scroll_top_;
  /** A remount's target offset, set each frame until the new rows are laid out and reach it. */
  std::optional<float> restore_scroll_top_;
  int restore_scroll_frames_ = 0;
  float last_messages_scroll_height_ = 0.f;
  float last_client_height_ = 0.f;
  int unread_while_scrolled_ = 0;
  /** JumpToMessage's row, scrolled to on the next layouts until its place stops moving. */
  std::string pending_jump_id_;
  int pending_jump_frames_ = 0;
  int pending_jump_stable_frames_ = 0;
  float pending_jump_last_top_ = -1.f;
  /** The row a jump landed on, tinted (message-row-jump-target) until `flash_until_` so the eye finds it. */
  std::string flash_id_;
  std::chrono::steady_clock::time_point flash_until_{};
  void KeepJumpFlash();
  void ClearJumpFlash();
  ConversationsFacade* facade_ = nullptr;
  RowBuilder rows_;
  RowDecorator decorator_;
};

} // namespace pbr
