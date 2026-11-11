#include <stdexcept>
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/chat/ChatSearchRows.h"

#include "foundation/i18n/LocalizationService.h"
#include "common/chat/MessagingLimits.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/NavBadgeFormat.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

void ChatTranscriptScroller::BindConversationsFacade(ConversationsFacade* facade) {
  facade_ = facade;
}

ChatTranscriptScroller::ChatTranscriptScroller(ui::Context*& context, View view, bool& messaging_ready)
    : context_(context), view_(view), messaging_ready_(messaging_ready) {}

std::vector<MessageDisplayRow> ChatTranscriptScroller::Rows(const std::string& thread_id,
                                                            const std::optional<int64_t> oldest_inclusive,
                                                            const std::optional<int64_t> newest_inclusive) const {
  std::vector<MessageDisplayRow> rows =
      rows_ ? rows_(thread_id, oldest_inclusive, newest_inclusive) : std::vector<MessageDisplayRow>{};
  if (decorator_) {
    decorator_(rows);
  }
  return rows;
}

void ChatTranscriptScroller::Redecorate() {
  for (MessageDisplayRow& row : view_.messages) {
    ClearSearchMarks(row);
  }
  if (decorator_) {
    decorator_(view_.messages);
  }
  if (dirty_turns_) {
    dirty_turns_();
  }
}

void ChatTranscriptScroller::Reset() {
  pinned_to_bottom_ = true;
  last_client_height_ = 0.f;
  pending_scroll_to_bottom_ = false;
  pending_scroll_settle_frames_ = 0;
  pending_scroll_attempts_ = 0;
  settle_scroll_height_ = -1.f;
  suppress_scroll_handler_ = false;
  loading_older_local_ = false;
  has_more_local_history_ = false;
  scroll_thread_id_.clear();
  loaded_min_display_order_.reset();
  loaded_max_display_order_.reset();
  pending_scroll_height_before_.reset();
  pending_scroll_top_before_.reset();
  reading_scroll_top_.reset();
  restore_scroll_top_.reset();
  last_messages_scroll_height_ = 0.f;
  unread_while_scrolled_ = 0;
  pending_jump_id_.clear();
  flash_id_.clear(); // the rows it named are gone
  view_.show_jump_to_latest = false;
  view_.jump_to_latest_count = "";
}

ui::Element* ChatTranscriptScroller::FindMessagesScrollElement() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return nullptr;
  }
  return context_->GetDocument(0)->GetElementById("chat-messages");
}

void ChatTranscriptScroller::UpdateJumpToLatestCount() {
  view_.jump_to_latest_count = unread_while_scrolled_ > 0 ? FormatBadgeCount(unread_while_scrolled_).c_str() : "";
}

void ChatTranscriptScroller::SetShowJumpToLatest(bool show) {
  if (view_.show_jump_to_latest == show && !show) {
    return;
  }
  view_.show_jump_to_latest = show;
  if (show) {
    UpdateJumpToLatestCount();
  } else {
    unread_while_scrolled_ = 0;
    view_.jump_to_latest_count = "";
  }
  DataModelHost::Instance().Dirty("chat", "show_jump_to_latest");
  DataModelHost::Instance().Dirty("chat", "jump_to_latest_count");
}

void ChatTranscriptScroller::RequestScrollToLatest() {
  pending_jump_id_.clear();
  reading_scroll_top_.reset();
  restore_scroll_top_.reset();
  pinned_to_bottom_ = true;
  pending_scroll_to_bottom_ = true;
  pending_scroll_settle_frames_ = 0;
  pending_scroll_attempts_ = 0;
  settle_scroll_height_ = -1.f;
  last_messages_scroll_height_ = 0.f;
  SetShowJumpToLatest(false);
}

float ChatTranscriptScroller::ScrollTopAfterViewportResize(const float top, const float old_client_height,
                                                           const float new_client_height, const float scroll_height,
                                                           const bool pinned_to_bottom) {
  const float max_top = std::max(0.f, scroll_height - new_client_height);
  if (pinned_to_bottom) {
    return max_top;
  }
  // Keep the bottom edge of what was in view where it was: the content moves with the composer.
  return std::clamp(top + (old_client_height - new_client_height), 0.f, max_top);
}

void ChatTranscriptScroller::ScrollMessagesToBottom() {
  ui::Element* el = FindMessagesScrollElement();
  if (!el) {
    return;
  }
  const float max_top = std::max(0.f, el->GetScrollHeight() - el->GetClientHeight());
  suppress_scroll_handler_ = true;
  el->SetScrollTop(max_top);
  suppress_scroll_handler_ = false;
  pinned_to_bottom_ = true;
  last_messages_scroll_height_ = el->GetScrollHeight();
  SetShowJumpToLatest(false);
}

void ChatTranscriptScroller::ApplyPolicy() {
  ui::Element* el = FindMessagesScrollElement();
  if (!el) {
    return;
  }

  if (restore_scroll_top_.has_value()) {
    // After a remount the rows are laid out a frame or two later, and an offset past the content is clamped away:
    // set it each frame until the content reaches it.
    const float client = el->GetClientHeight();
    const float max_top = std::max(0.f, el->GetScrollHeight() - client);
    suppress_scroll_handler_ = true;
    el->SetScrollTop(std::min(*restore_scroll_top_, max_top));
    suppress_scroll_handler_ = false;
    if (client > 0.f) {
      last_client_height_ = client;
    }
    last_messages_scroll_height_ = el->GetScrollHeight();
    if ((client > 0.f && max_top >= *restore_scroll_top_ - 0.5f) || ++restore_scroll_frames_ > 30) {
      restore_scroll_top_.reset();
    }
    return;
  }
  if (!pinned_to_bottom_ && el->GetClientHeight() > 0.f) {
    reading_scroll_top_ = el->GetScrollTop(); // what OnShellRemounted puts back
  }

  if (pending_scroll_height_before_.has_value() && pending_scroll_top_before_.has_value()) {
    const float delta = el->GetScrollHeight() - *pending_scroll_height_before_;
    suppress_scroll_handler_ = true;
    el->SetScrollTop(*pending_scroll_top_before_ + delta);
    suppress_scroll_handler_ = false;
    pending_scroll_height_before_.reset();
    pending_scroll_top_before_.reset();
    last_messages_scroll_height_ = el->GetScrollHeight();
  }

  KeepJumpFlash();
  if (ApplyPendingJump(el)) {
    return;
  }

  const float scroll_height = el->GetScrollHeight();
  // The viewport changed height (the on-screen keyboard came up or went away): the scroll offset alone
  // would leave the newest messages under the keyboard, so re-anchor before judging near-bottom.
  // A 0 reading (element not laid out yet, e.g. right after a remount) is not a size change.
  const float client_height = el->GetClientHeight();
  if (client_height > 0.f) {
    if (last_client_height_ > 0.f && std::abs(client_height - last_client_height_) > 0.5f) {
      suppress_scroll_handler_ = true;
      el->SetScrollTop(ScrollTopAfterViewportResize(el->GetScrollTop(), last_client_height_, client_height,
                                                    scroll_height, pinned_to_bottom_));
      suppress_scroll_handler_ = false;
    }
    last_client_height_ = client_height;
  }
  const float max_top = std::max(0.f, scroll_height - client_height);
  const float top = el->GetScrollTop();
  constexpr float kNearBottomPx = 64.f;
  const bool near_bottom = max_top <= 0.5f || (max_top - top) <= kNearBottomPx;

  if (pending_scroll_to_bottom_) {
    ScrollMessagesToBottom();
    const float h = el->GetScrollHeight();
    const float settled_max = std::max(0.f, h - el->GetClientHeight());
    const float settled_top = el->GetScrollTop();
    const bool at_bottom = settled_max <= 0.5f || (settled_max - settled_top) <= 1.f;
    const bool height_stable = settle_scroll_height_ >= 0.f && std::abs(h - settle_scroll_height_) < 0.5f;

    if (!view_.has_turns) {
      pending_scroll_to_bottom_ = false;
      pending_scroll_settle_frames_ = 0;
      settle_scroll_height_ = -1.f;
    } else if (at_bottom && height_stable) {
      ++pending_scroll_settle_frames_;
      if (pending_scroll_settle_frames_ >= 3) {
        pending_scroll_to_bottom_ = false;
        pending_scroll_settle_frames_ = 0;
        settle_scroll_height_ = -1.f;
      }
    } else {
      settle_scroll_height_ = h;
      pending_scroll_settle_frames_ = 0;
      ++pending_scroll_attempts_;
      if (pending_scroll_attempts_ > 60) {
        pending_scroll_to_bottom_ = false;
        pending_scroll_attempts_ = 0;
        settle_scroll_height_ = -1.f;
      }
    }
    last_messages_scroll_height_ = h;
    return;
  }

  pending_scroll_attempts_ = 0;

  if (pinned_to_bottom_) {
    if (scroll_height > last_messages_scroll_height_ + 0.5f) {
      ScrollMessagesToBottom();
      return;
    }
    if (!near_bottom) {
      pinned_to_bottom_ = false;
      last_messages_scroll_height_ = scroll_height;
      return;
    }
    if (view_.show_jump_to_latest) {
      SetShowJumpToLatest(false);
    }
    last_messages_scroll_height_ = scroll_height;
    return;
  }

  if (near_bottom && max_top > 0.5f) {
    pinned_to_bottom_ = true;
    if (view_.show_jump_to_latest) {
      SetShowJumpToLatest(false);
    }
  }
  last_messages_scroll_height_ = scroll_height;
}

void ChatTranscriptScroller::MaybeLoadOlderLocalHistory() {
  if (loading_older_local_ || !has_more_local_history_ || !messaging_ready_ || pending_scroll_to_bottom_ ||
      !pending_jump_id_.empty()) {
    return;
  }
  ui::Element* el = FindMessagesScrollElement();
  if (!el) {
    return;
  }
  const float max_top = std::max(0.f, el->GetScrollHeight() - el->GetClientHeight());
  if (max_top <= 1.f || el->GetScrollTop() > 72.f) {
    return;
  }
  LoadOlderLocalHistory();
}

void ChatTranscriptScroller::LoadOlderLocalHistory() {
  if (loading_older_local_ || !messaging_ready_ || view_.messages.empty()) {
    return;
  }
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string{};
  if (thread_id.empty()) {
    return;
  }

  const int64_t oldest = view_.messages.front().display_order;
  if (!facade_ || !facade_->HasLocalMessagesBefore(thread_id, oldest)) {
    has_more_local_history_ = false;
    return;
  }

  ui::Element* el = FindMessagesScrollElement();
  if (el) {
    pending_scroll_height_before_ = el->GetScrollHeight();
    pending_scroll_top_before_ = el->GetScrollTop();
  }

  loading_older_local_ = true;
  auto older = facade_ ? facade_->GetMessagesPage(thread_id, oldest, kDefaultMessagesPageSize)
                       : Roe<std::vector<ThreadMessage>>::error(Error("chat port unavailable"));
  if (!older || older->empty()) {
    has_more_local_history_ = false;
    loading_older_local_ = false;
    pending_scroll_height_before_.reset();
    pending_scroll_top_before_.reset();
    return;
  }
  loaded_min_display_order_ = older->front().display_order;
  has_more_local_history_ = facade_ && facade_->HasLocalMessagesBefore(thread_id, *loaded_min_display_order_);

  view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
  if (TrimDomWindow(view_.messages, /*pinned_to_bottom=*/false, loaded_min_display_order_, loaded_max_display_order_,
                    has_more_local_history_)) {
    pinned_to_bottom_ = false;
    SetShowJumpToLatest(true);
    // Rebuild with new max bound so store sync stays coherent.
    view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
  }
  view_.has_turns = !view_.messages.empty();
  if (dirty_turns_) {
    dirty_turns_();
  }
  loading_older_local_ = false;
}

void ChatTranscriptScroller::OnMessagesScroll() {
  if (suppress_scroll_handler_) {
    return;
  }
  ui::Element* el = FindMessagesScrollElement();
  if (!el) {
    return;
  }
  const float max_top = std::max(0.f, el->GetScrollHeight() - el->GetClientHeight());
  const float top = el->GetScrollTop();
  constexpr float kNearBottomPx = 64.f;
  const bool near_bottom = max_top <= 0.f || (max_top - top) <= kNearBottomPx;
  if (near_bottom) {
    pinned_to_bottom_ = true;
    SetShowJumpToLatest(false);
  } else {
    pinned_to_bottom_ = false;
    // Scrolled more than half a screen up: offer the way back to the newest message.
    if (!view_.show_jump_to_latest && (max_top - top) > el->GetClientHeight() * 0.5f) {
      SetShowJumpToLatest(true);
    }
  }
  MaybeLoadOlderLocalHistory();
}

void ChatTranscriptScroller::OnJumpToLatest() {
  loaded_max_display_order_.reset();
  loaded_min_display_order_.reset();
  if (facade_) {
    const std::string thread_id = facade_->ActiveThreadId();
    if (!thread_id.empty()) {
      view_.messages = Rows(thread_id, std::nullopt, std::nullopt);
      view_.has_turns = !view_.messages.empty();
      if (!view_.messages.empty()) {
        loaded_min_display_order_ = view_.messages.front().display_order;
        has_more_local_history_ = facade_->HasLocalMessagesBefore(thread_id, view_.messages.front().display_order);
      }
      if (dirty_turns_) {
        dirty_turns_();
      }
    }
  }
  RequestScrollToLatest();
  ScrollMessagesToBottom();
}

ChatTranscriptScroller::JumpWindow ChatTranscriptScroller::WindowForJump(const int64_t order,
                                                                         const std::optional<int64_t> loaded_min,
                                                                         const std::optional<int64_t> loaded_max) {
  const bool older = !loaded_min || order < *loaded_min;
  const bool newer = loaded_max && order > *loaded_max;
  if (!older && !newer) {
    return {false, loaded_min, loaded_max};
  }
  // Drawn from the message on (the newest dropped when it gets too long, as when paging up).
  return {true, order, std::nullopt};
}

ChatTranscriptScroller::JumpResult ChatTranscriptScroller::JumpToMessage(const std::string& message_id) {
  const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string{};
  if (thread_id.empty() || message_id.empty()) {
    return JumpResult::Unavailable;
  }
  auto message = facade_->GetMessage(thread_id, message_id);
  if (!message) {
    return JumpResult::Unavailable; // the store did not answer: not the same as "no such message"
  }
  if (!*message) {
    return JumpResult::NotFound;
  }
  const int64_t order = (*message)->display_order;
  const JumpWindow window = WindowForJump(order, loaded_min_display_order_, loaded_max_display_order_);
  if (window.refetch) {
    loaded_min_display_order_ = window.loaded_min;
    loaded_max_display_order_ = window.loaded_max;
    view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
    if (TrimDomWindow(view_.messages, /*pinned_to_bottom=*/false, loaded_min_display_order_, loaded_max_display_order_,
                      has_more_local_history_)) {
      view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
    }
    if (!view_.messages.empty()) {
      loaded_min_display_order_ = view_.messages.front().display_order;
      has_more_local_history_ = facade_->HasLocalMessagesBefore(thread_id, *loaded_min_display_order_);
    }
    view_.has_turns = !view_.messages.empty();
    if (dirty_turns_) {
      dirty_turns_();
    }
  }
  pending_scroll_to_bottom_ = false;
  pinned_to_bottom_ = false;
  pending_jump_id_ = message_id;
  pending_jump_frames_ = 0;
  pending_jump_stable_frames_ = 0;
  pending_jump_last_top_ = -1.f;
  const bool is_tail = !view_.messages.empty() && view_.messages.back().message_id == message_id.c_str();
  SetShowJumpToLatest(!is_tail || loaded_max_display_order_.has_value());
  return JumpResult::Jumped;
}

bool ChatTranscriptScroller::ApplyPendingJump(ui::Element* el) {
  if (pending_jump_id_.empty()) {
    return false;
  }
  constexpr int kMaxJumpFrames = 60;
  if (++pending_jump_frames_ > kMaxJumpFrames) {
    pending_jump_id_.clear(); // never laid out (gone meanwhile), or never still: leave the view where it is
    return false;
  }
  ui::Element* row = context_->GetDocument(0)->QuerySelector(("[message-id=\"" + pending_jump_id_ + "\"]").c_str());
  if (!row) {
    return true; // not mounted yet (a remount is coming)
  }
  if (flash_id_ != pending_jump_id_) {
    ClearJumpFlash();
  }
  // Where the jump landed is tinted for a moment (a quote's original, a search hit, a mention): until 1.5 s
  // after its place stops moving.
  flash_id_ = pending_jump_id_;
  flash_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  row->SetClass("message-row-jump-target", true);
  suppress_scroll_handler_ = true;
  row->ScrollIntoView(true);
  suppress_scroll_handler_ = false;
  last_messages_scroll_height_ = el->GetScrollHeight();
  const float top = el->GetScrollTop();
  // Rows above it can still grow (images, fonts): keep it in view until its place holds for a few layouts.
  pending_jump_stable_frames_ = std::abs(top - pending_jump_last_top_) < 0.5f ? pending_jump_stable_frames_ + 1 : 0;
  pending_jump_last_top_ = top;
  if (pending_jump_stable_frames_ >= 3) {
    pending_jump_id_.clear();
  }
  return true;
}

void ChatTranscriptScroller::KeepJumpFlash() {
  if (flash_id_.empty()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= flash_until_ || !context_ || context_->GetNumDocuments() == 0) {
    ClearJumpFlash();
    return;
  }
  // A rebuilt row (new message, download state, remount) comes back without the class: put it on again.
  if (ui::Element* row = context_->GetDocument(0)->QuerySelector(("[message-id=\"" + flash_id_ + "\"]").c_str())) {
    row->SetClass("message-row-jump-target", true);
  }
  // An idle chat draws no frames: wake once more when the tint is due to go.
  context_->RequestNextUpdate(std::chrono::duration<double>(flash_until_ - now).count());
}

void ChatTranscriptScroller::ClearJumpFlash() {
  if (flash_id_.empty()) {
    return;
  }
  if (context_ && context_->GetNumDocuments() > 0) {
    if (ui::Element* row = context_->GetDocument(0)->QuerySelector(("[message-id=\"" + flash_id_ + "\"]").c_str())) {
      row->SetClass("message-row-jump-target", false);
    }
  }
  flash_id_.clear();
}

void ChatTranscriptScroller::OnShellRemounted() {
  if (pinned_to_bottom_ && view_.has_turns) {
    pending_scroll_to_bottom_ = true;
    pending_scroll_settle_frames_ = 0;
    pending_scroll_attempts_ = 0;
    settle_scroll_height_ = -1.f;
    last_messages_scroll_height_ = 0.f;
  } else if (reading_scroll_top_.value_or(0.f) > 0.f) {
    // Reading older messages: the new DOM starts at the top (MountInner restores the offset before the rows exist).
    restore_scroll_top_ = reading_scroll_top_;
    restore_scroll_frames_ = 0;
    last_client_height_ = 0.f; // the first reading of the new element is not a viewport resize
  }
  ApplyPolicy();
}

bool ChatTranscriptScroller::BeginDisplaySync(const std::string& thread_id) {
  const bool thread_changed = thread_id != scroll_thread_id_;
  if (thread_changed) {
    pending_jump_id_.clear();
    flash_id_.clear(); // another chat's rows
    scroll_thread_id_ = thread_id;
    loaded_min_display_order_.reset();
    loaded_max_display_order_.reset();
    unread_while_scrolled_ = 0;
    RequestScrollToLatest();
  }
  return thread_changed;
}

void ChatTranscriptScroller::EndDisplaySync(bool thread_changed, const std::string& prev_tail_id, size_t prev_count) {
  if (!view_.messages.empty()) {
    if (TrimDomWindow(view_.messages, pinned_to_bottom_, loaded_min_display_order_, loaded_max_display_order_,
                      has_more_local_history_)) {
      if (pinned_to_bottom_ && facade_) {
        const std::string thread_id = facade_->ActiveThreadId();
        view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
        view_.has_turns = !view_.messages.empty();
      } else if (!pinned_to_bottom_) {
        SetShowJumpToLatest(true);
      }
    }
    if (!view_.messages.empty()) {
      loaded_min_display_order_ = view_.messages.front().display_order;
      if (!loaded_max_display_order_.has_value()) {
        // Tip-aligned window: max is the current tip (implicit).
      } else {
        loaded_max_display_order_ = view_.messages.back().display_order;
      }
      const std::string thread_id = facade_ ? facade_->ActiveThreadId() : std::string{};
      has_more_local_history_ =
          facade_ && facade_->HasLocalMessagesBefore(thread_id, view_.messages.front().display_order);
    }
  } else {
    loaded_min_display_order_.reset();
    loaded_max_display_order_.reset();
    has_more_local_history_ = false;
  }

  if (thread_changed || pinned_to_bottom_) {
    RequestScrollToLatest();
  } else {
    const std::string new_tail_id =
        view_.messages.empty() ? std::string() : std::string(view_.messages.back().message_id.c_str());
    if (!new_tail_id.empty() && new_tail_id != prev_tail_id && view_.messages.size() > prev_count) {
      unread_while_scrolled_ += static_cast<int>(view_.messages.size() - prev_count);
      SetShowJumpToLatest(true);
    }
  }
}

bool ChatTranscriptScroller::TrimDomWindow(std::vector<MessageDisplayRow>& messages, bool pinned_to_bottom,
                                           std::optional<int64_t>& loaded_min, std::optional<int64_t>& loaded_max,
                                           bool& has_more_local_history) {
  if (messages.size() <= kMaxMessagesDomWindow) {
    return false;
  }
  if (pinned_to_bottom) {
    const size_t drop = messages.size() - (kMaxMessagesDomWindow - kMessagesDomWindowTrimBatch);
    messages.erase(messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(drop));
    loaded_min = messages.front().display_order;
    loaded_max.reset();
    has_more_local_history = true;
    return true;
  }
  messages.resize(kMaxMessagesDomWindow);
  loaded_min = messages.front().display_order;
  loaded_max = messages.back().display_order;
  return true;
}

void ChatTranscriptScroller::CaptureScrollBeforePrependIfUnpinned() {
  ui::Element* el = FindMessagesScrollElement();
  if (el && !pinned_to_bottom_) {
    pending_scroll_height_before_ = el->GetScrollHeight();
    pending_scroll_top_before_ = el->GetScrollTop();
  }
}

void ChatTranscriptScroller::ExpandLoadedMinFromOlderPage(const std::string& thread_id, int64_t before_display_order) {
  auto older = facade_ ? facade_->GetMessagesPage(thread_id, before_display_order, kDefaultMessagesPageSize)
                       : Roe<std::vector<ThreadMessage>>::error(Error("chat port unavailable"));
  if (older && !older->empty()) {
    loaded_min_display_order_ = older->front().display_order;
  }
}

} // namespace pbr
