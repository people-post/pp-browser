#pragma once

#include "domain/ui/ChatWidgetTypes.h"
#include "feature/messaging/MessagingFacade.h"

#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

/** Follow-tail / scroll-up paging for the chat transcript (D031 + viewport policy). */
class ChatTranscriptScroller {
public:
  struct View {
    bool& show_jump_to_latest;
    Rml::String& jump_to_latest_label;
    std::vector<MessageDisplayRow>& messages;
    bool& has_turns;
  };

  ChatTranscriptScroller(Rml::Context*& context, View view, bool& messaging_ready);
  void BindMessagingFacade(MessagingFacade* facade);

  void SetDirtyTurns(std::function<void()> dirty_turns) { dirty_turns_ = std::move(dirty_turns); }

  void Reset();
  void RequestScrollToLatest();
  void ApplyPolicy();
  void OnMessagesScroll();
  void OnJumpToLatest();
  /** SyncLayout remounts DOM — re-arm follow-tail when still pinned. */
  void OnShellRemounted();

  bool IsPinnedToBottom() const { return pinned_to_bottom_; }
  const std::optional<int64_t>& LoadedMinDisplayOrder() const { return loaded_min_display_order_; }
  std::optional<int64_t>& LoadedMinDisplayOrder() { return loaded_min_display_order_; }
  const std::optional<int64_t>& LoadedMaxDisplayOrder() const { return loaded_max_display_order_; }

  /**
   * Cap `messages` to kMaxMessagesDomWindow. When pinned, drop oldest and raise loaded_min.
   * When unpinned (history), drop newest and set loaded_max. Returns true if trimmed.
   */
  static bool TrimDomWindow(std::vector<MessageDisplayRow>& messages, bool pinned_to_bottom,
                            std::optional<int64_t>& loaded_min, std::optional<int64_t>& loaded_max,
                            bool& has_more_local_history);

  /** Call at start of SyncDisplayFromThread; returns true if active thread id changed. */
  bool BeginDisplaySync(const std::string& thread_id);
  /** After rebuilding messages; updates watermarks and pin / jump-FAB. */
  void EndDisplaySync(bool thread_changed, const std::string& prev_tail_id, size_t prev_count);

  /** Capture scroll offset before peer older-history expands the window. */
  void CaptureScrollBeforePrependIfUnpinned();
  void ExpandLoadedMinFromOlderPage(const std::string& thread_id, int64_t before_display_order);

private:
  Rml::Element* FindMessagesScrollElement() const;
  void UpdateJumpToLatestLabel();
  void SetShowJumpToLatest(bool show);
  void ScrollMessagesToBottom();
  void MaybeLoadOlderLocalHistory();
  void LoadOlderLocalHistory();

  Rml::Context*& context_;
  View view_;
  bool& messaging_ready_;
  std::function<void()> dirty_turns_;

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
  float last_messages_scroll_height_ = 0.f;
  int unread_while_scrolled_ = 0;
  MessagingFacade* facade_ = nullptr;
};

} // namespace pbr
