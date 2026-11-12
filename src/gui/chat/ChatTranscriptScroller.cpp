#include <stdexcept>
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/chat/PendingUploadRows.h"
#include "gui/chat/ChatSearchRows.h"

#include "foundation/i18n/LocalizationService.h"
#include "common/chat/MessagingLimits.h"
#include "gui/shell/DataModelHost.h"
#include "gui/UiFrameStats.h"
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
  if (!facade) {
    reads_ = {};
    return;
  }
  reads_.active_thread_id = [facade]() { return facade->ActiveThreadId(); };
  reads_.messages_page = [facade](const std::string& thread_id, std::optional<int64_t> before, int limit) {
    return facade->GetMessagesPage(thread_id, before, limit);
  };
  reads_.has_messages_before = [facade](const std::string& thread_id, int64_t before_display_order) {
    return facade->HasLocalMessagesBefore(thread_id, before_display_order);
  };
  reads_.message = [facade](const std::string& thread_id, const std::string& message_id) {
    return facade->GetMessage(thread_id, message_id);
  };
}

std::string ChatTranscriptScroller::ActiveThreadId() const {
  return reads_.active_thread_id ? reads_.active_thread_id() : std::string{};
}

bool ChatTranscriptScroller::HasMessagesBefore(const std::string& thread_id, const int64_t before_display_order) const {
  return reads_.has_messages_before && reads_.has_messages_before(thread_id, before_display_order);
}

Roe<std::vector<ThreadMessage>> ChatTranscriptScroller::MessagesPage(const std::string& thread_id,
                                                                     const std::optional<int64_t> before,
                                                                     const int limit) const {
  return reads_.messages_page ? reads_.messages_page(thread_id, before, limit)
                              : Roe<std::vector<ThreadMessage>>::error(Error("chat port unavailable"));
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
  pending_anchor_id_.clear();
  reading_scroll_top_.reset();
  restore_scroll_top_.reset();
  last_messages_scroll_height_ = 0.f;
  unread_while_scrolled_ = 0;
  first_screen_on_return_ = false;
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
  // Hidden (a full-screen call page covers the chat, ShellBaseHide): nothing in it is laid out, so every reading is 0.
  // Wait — no restore frame or scroll attempt is spent on it — and carry on once it shows again.
  if (!el->IsVisible(true)) {
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
    // The anchor row (LoadOlderLocalHistory) stays where it was on screen. The height change says the same only when
    // nothing left the window: a full window trims as many of its newest rows as the page added, and the content keeps
    // its height.
    float target = *pending_scroll_top_before_ + (el->GetScrollHeight() - *pending_scroll_height_before_);
    if (const float offset = RowContentOffset(el, pending_anchor_id_); offset >= 0.f) {
      target = *pending_scroll_top_before_ + (offset - pending_anchor_offset_);
    }
    suppress_scroll_handler_ = true;
    el->SetScrollTop(target);
    suppress_scroll_handler_ = false;
    pending_scroll_height_before_.reset();
    pending_scroll_top_before_.reset();
    pending_anchor_id_.clear();
    last_messages_scroll_height_ = el->GetScrollHeight();
    if (page_reading_ && !page_reading_->fill && !page_reading_->frames_before_fix) {
      // A frame drawn between the page and this fix showed the rows at the old offset: the jump the person sees.
      page_reading_->frames_before_fix = UiFrameStats::DrawnFrames() > page_frames_at_load_ ? 1 : 0;
    }
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

  if (MaybeFillScreen(client_height, scroll_height, max_top)) {
    return;
  }

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

float ChatTranscriptScroller::RowContentOffset(ui::Element* el, const std::string& message_id) const {
  if (message_id.empty() || !context_ || context_->GetNumDocuments() == 0) {
    return -1.f;
  }
  ui::Element* row = context_->GetDocument(0)->QuerySelector(("[message-id=\"" + message_id + "\"]").c_str());
  if (!row) {
    return -1.f;
  }
  // Where the row sits in the content, whatever the scroll offset: from the transcript's top edge, plus the offset.
  return row->GetAbsoluteOffset(ui::BoxArea::Border).y - el->GetAbsoluteOffset(ui::BoxArea::Border).y +
         el->GetScrollTop();
}

bool ChatTranscriptScroller::MaybeFillScreen(const float client_height, const float scroll_height,
                                             const float max_top) {
  // The rows end less than the top's paging threshold above the screen (a tall window, a tablet, short rows): no scroll
  // would ever reach the page above, so it is loaded now. At most 100 rows, so the page never makes the window trim.
  constexpr float kTopPagingPx = 72.f; // MaybeLoadOlderLocalHistory's
  constexpr int kMaxPagesWithoutRows = 3;
  if (client_height <= 0.f || scroll_height <= 0.f || max_top > kTopPagingPx || !has_more_local_history_ ||
      loading_older_local_ || !messaging_ready_ || !pending_jump_id_.empty() || restore_scroll_top_ ||
      pending_scroll_height_before_ || pending_scroll_top_before_ || !pinned_to_bottom_ ||
      view_.messages.size() > kMaxMessagesDomWindow - kDefaultMessagesPageSize ||
      fill_pages_without_rows_ >= kMaxPagesWithoutRows) {
    return false;
  }
  const size_t rows_before = view_.messages.size();
  if (!LoadOlderLocalHistory(/*fill=*/true)) {
    return false; // nothing loaded (has_more_local_history_ now says so): no landing again, nothing counted
  }
  // Back to the newest message the way an open lands there, not by the scroll-up position fix.
  pending_scroll_height_before_.reset();
  pending_scroll_top_before_.reset();
  pending_anchor_id_.clear();
  // Pages of reactions and edits only add no row: after three in a row it waits for the person to scroll (a page that
  // adds rows, by either path, starts the count again: LoadOlderLocalHistory).
  if (view_.messages.size() <= rows_before) {
    ++fill_pages_without_rows_;
  }
  RequestScrollToLatest();
  return true;
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
  // A page of rows that do not show (reactions, edits) adds nothing and leaves the offset where it was: at the very top
  // no further scroll event would come. Read on at once, three pages at most; then the next gesture goes on.
  constexpr int kMaxPagesPerScroll = 3;
  // Progress is an older first row: a full window keeps its 200 rows (the newest are trimmed), so the count says nothing.
  for (int page = 0; page < kMaxPagesPerScroll; ++page) {
    const int64_t front_before = view_.messages.front().display_order;
    if (!LoadOlderLocalHistory() || view_.messages.empty() || view_.messages.front().display_order < front_before ||
        !has_more_local_history_) {
      return;
    }
  }
}

bool ChatTranscriptScroller::LoadOlderLocalHistory(const bool fill) {
  if (loading_older_local_ || !messaging_ready_ || view_.messages.empty()) {
    return false;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return false;
  }

  // From the window's lower bound when a page of rows that do not show moved it past the first row shown (else that
  // page would be read again each time and the history never passed).
  int64_t oldest = view_.messages.front().display_order;
  if (loaded_min_display_order_ && *loaded_min_display_order_ < oldest) {
    oldest = *loaded_min_display_order_;
  }
  if (!HasMessagesBefore(thread_id, oldest)) {
    has_more_local_history_ = false;
    return false;
  }

  ui::Element* el = FindMessagesScrollElement();
  if (el) {
    pending_scroll_height_before_ = el->GetScrollHeight();
    pending_scroll_top_before_ = el->GetScrollTop();
    // The second row: the first one changes shape once older rows of its day and sender come in (its date strip, the
    // sender's name and the run's top margin go), the second only depends on the first, which stays. On a page that
    // showed nothing the rows are unchanged, so a second page in the same gesture records the same row and offset.
    const MessageDisplayRow& anchor = view_.messages.size() >= 2 ? view_.messages[1] : view_.messages.front();
    const std::string anchor_id(anchor.message_id.c_str());
    if (const float offset = RowContentOffset(el, anchor_id); offset >= 0.f) {
      pending_anchor_id_ = anchor_id;
      pending_anchor_offset_ = offset;
    }
  }

  loading_older_local_ = true;
  const size_t rows_before = view_.messages.size();
  // A scroll to the top loads a short page (its frame is the one people feel); a fill keeps its 100-row arithmetic.
  const size_t page_size = fill ? kDefaultMessagesPageSize : kChatOlderPageMessages;
  auto older = MessagesPage(thread_id, oldest, static_cast<int>(page_size));
  if (!older || older->empty()) {
    has_more_local_history_ = false;
    loading_older_local_ = false;
    pending_scroll_height_before_.reset();
    pending_scroll_top_before_.reset();
    pending_anchor_id_.clear();
    return false;
  }
  first_screen_pending_ = false;
  loaded_min_display_order_ = older->front().display_order;
  has_more_local_history_ = HasMessagesBefore(thread_id, *loaded_min_display_order_);

  view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
  if (TrimDomWindow(view_.messages, /*pinned_to_bottom=*/false, loaded_min_display_order_, loaded_max_display_order_,
                    has_more_local_history_)) {
    pinned_to_bottom_ = false;
    SetShowJumpToLatest(true);
    // Rebuild with new max bound so store sync stays coherent.
    view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
  }
  view_.has_turns = !view_.messages.empty();
  if (view_.messages.size() > rows_before) {
    fill_pages_without_rows_ = 0;
  }
  page_reading_ = ChatPageReading{
      .added = static_cast<int64_t>(view_.messages.size()) - static_cast<int64_t>(rows_before), .fill = fill};
  page_frames_at_load_ = UiFrameStats::DrawnFrames();
  if (dirty_turns_) {
    dirty_turns_();
  }
  loading_older_local_ = false;
  if (on_older_loaded_) {
    on_older_loaded_();
  }
  return true;
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
  first_screen_pending_ = false;
  loaded_max_display_order_.reset();
  loaded_min_display_order_.reset();
  if (const std::string thread_id = ActiveThreadId(); !thread_id.empty()) {
    view_.messages = Rows(thread_id, std::nullopt, std::nullopt);
    view_.has_turns = !view_.messages.empty();
    if (!view_.messages.empty()) {
      loaded_min_display_order_ = view_.messages.front().display_order;
      has_more_local_history_ = HasMessagesBefore(thread_id, view_.messages.front().display_order);
    }
    if (dirty_turns_) {
      dirty_turns_();
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
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty() || message_id.empty() || !reads_.message) {
    return JumpResult::Unavailable;
  }
  auto message = reads_.message(thread_id, message_id);
  if (!message) {
    return JumpResult::Unavailable; // the store did not answer: not the same as "no such message"
  }
  if (!*message) {
    return JumpResult::NotFound;
  }
  const int64_t order = (*message)->display_order;
  first_screen_pending_ = false;
  first_screen_on_return_ = false; // the jump decides where the chat opens, not the first screen
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
      has_more_local_history_ = HasMessagesBefore(thread_id, *loaded_min_display_order_);
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
  // The picture being sent at the end is not a message: the newest real row counts as the tail.
  const bool is_tail = PendingUploadRows::RealTail(view_.messages).first == message_id;
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
    first_screen_on_return_ = false; // the offset is put back, not the first screen
    last_client_height_ = 0.f; // the first reading of the new element is not a viewport resize
  }
  ApplyPolicy();
}

void ChatTranscriptScroller::OnChatLeft() {
  first_screen_on_return_ = pinned_to_bottom_ && !loaded_max_display_order_ && pending_jump_id_.empty();
}

bool ChatTranscriptScroller::BeginDisplaySync(const std::string& thread_id, const size_t first_screen_messages) {
  const bool thread_changed = thread_id != scroll_thread_id_;
  // The same chat opened again after it was left on its newest messages: its window goes back to the first screen
  // (only the window and the first-screen state; for everything else it is the same chat).
  // Not when a jump, a restored offset, a position off the bottom or a window short of the newest message came
  // after the mark: that state is what the user asked for.
  const bool moved_on = !pending_jump_id_.empty() || restore_scroll_top_ || !pinned_to_bottom_ ||
                        loaded_max_display_order_.has_value();
  const bool reopened = !thread_changed && first_screen_on_return_ && !moved_on;
  first_screen_on_return_ = false;
  if (reopened) {
    loaded_min_display_order_.reset();
    first_screen_ = FirstScreen::Miss;
    first_screen_pending_ = first_screen_messages > 0;
    first_screen_retries_ = 0;
    fill_pages_without_rows_ = 0;
    RequestScrollToLatest();
  }
  if (thread_changed) {
    pending_jump_id_.clear();
    flash_id_.clear(); // another chat's rows
    scroll_thread_id_ = thread_id;
    loaded_min_display_order_.reset();
    loaded_max_display_order_.reset();
    unread_while_scrolled_ = 0;
    first_screen_ = FirstScreen::Miss;
    first_screen_pending_ = first_screen_messages > 0;
    first_screen_retries_ = 0;
    fill_pages_without_rows_ = 0;
    page_reading_.reset(); // another chat's page
    RequestScrollToLatest();
  }
  // The first screen starts at the first_screen_messages-th newest message, decided once for the chat switched to. A
  // read that did not happen (not ready, store error) is tried on the next syncs, at most five, and only while the
  // window still ends at the newest message and sits exactly on the bottom: cutting it under someone who moved, or with
  // a jump or a remount restore pending, would move what they look at. Otherwise the first screen is given up (miss);
  // a jump, back to the latest or a page loaded also ends the wait.
  constexpr int kMaxFirstScreenRetries = 5;
  if (first_screen_pending_ && !thread_changed && !reopened) {
    if (first_screen_retries_ >= kMaxFirstScreenRetries || !pinned_to_bottom_ || loaded_max_display_order_ ||
        restore_scroll_top_ || !pending_jump_id_.empty() || !ViewOnTheBottom()) {
      first_screen_pending_ = false;
    } else {
      ++first_screen_retries_;
    }
  }
  if (first_screen_pending_) {
    std::optional<int64_t> oldest;
    first_screen_ = FirstScreenBound(thread_id, first_screen_messages, oldest);
    if (first_screen_ != FirstScreen::Miss) {
      first_screen_pending_ = false;
      if (oldest) {
        loaded_min_display_order_ = oldest;
      }
    }
  }
  return thread_changed;
}

bool ChatTranscriptScroller::ViewOnTheBottom() const {
  ui::Element* el = FindMessagesScrollElement();
  if (!el || !el->IsVisible(true) || el->GetClientHeight() <= 0.f || pending_scroll_to_bottom_) {
    return true; // not laid out or not shown (nothing moved yet), or still being held on the bottom
  }
  const float max_top = std::max(0.f, el->GetScrollHeight() - el->GetClientHeight());
  return max_top - el->GetScrollTop() <= 1.f;
}

ChatTranscriptScroller::FirstScreen ChatTranscriptScroller::FirstScreenBound(const std::string& thread_id,
                                                                            const size_t shown,
                                                                            std::optional<int64_t>& oldest) const {
  if (!messaging_ready_) {
    return FirstScreen::Miss;
  }
  // The first read takes only `shown` rows (all of them are shown in a chat of plain messages); short of shown
  // messages, a page of 100 at a time further back. Three reads at most.
  constexpr int kMaxReads = 3;
  size_t counted = 0;
  std::optional<int64_t> before;
  for (int read = 0; read < kMaxReads; ++read) {
    const size_t limit = read == 0 ? shown : kDefaultMessagesPageSize;
    auto rows = MessagesPage(thread_id, before, static_cast<int>(limit));
    if (!rows) {
      return FirstScreen::Miss;
    }
    for (auto row = rows->rbegin(); row != rows->rend(); ++row) {
      if (row->content_type == ChatContentType::Annotation) {
        continue; // folded onto its message, or never shown (AssembleTranscript)
      }
      if (++counted == shown) {
        oldest = row->display_order;
        return FirstScreen::Ok;
      }
    }
    if (rows->size() < limit) {
      return FirstScreen::Short; // the chat's beginning: all of it fits
    }
    before = rows->front().display_order;
  }
  // Three reads short of `shown`: nearly all of it is rows that do not show. A bound at the oldest row read could leave
  // the screen blank with nothing to page from; the old first page (which reads on until something shows) is kept.
  return FirstScreen::Short;
}

void ChatTranscriptScroller::EndDisplaySync(bool thread_changed, const std::string& prev_tail_id, size_t prev_count) {
  if (!view_.messages.empty()) {
    if (TrimDomWindow(view_.messages, pinned_to_bottom_, loaded_min_display_order_, loaded_max_display_order_,
                      has_more_local_history_)) {
      if (pinned_to_bottom_ && reads_.active_thread_id) {
        const std::string thread_id = ActiveThreadId();
        view_.messages = Rows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
        view_.has_turns = !view_.messages.empty();
      } else if (!pinned_to_bottom_) {
        SetShowJumpToLatest(true);
      }
    }
    if (!view_.messages.empty()) {
      // A bound at or before the first row stays: paging may have passed rows that do not show (reactions, edits), and
      // a sync (a new message, an AI answer streaming) must not send the next scroll back over them. Rows() draws the
      // same rows from either. A bound after the first row (none yet, or the window moved) takes the first row.
      const int64_t front = view_.messages.front().display_order;
      if (!loaded_min_display_order_ || *loaded_min_display_order_ > front) {
        loaded_min_display_order_ = front;
      }
      if (!loaded_max_display_order_.has_value()) {
        // Tip-aligned window: max is the current tip (implicit).
      } else {
        loaded_max_display_order_ = view_.messages.back().display_order;
      }
      has_more_local_history_ = HasMessagesBefore(ActiveThreadId(), *loaded_min_display_order_);
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
    pending_anchor_id_.clear(); // a peer's page: the height change, as before
  }
}

void ChatTranscriptScroller::ExpandLoadedMinFromOlderPage(const std::string& thread_id, int64_t before_display_order) {
  auto older = MessagesPage(thread_id, before_display_order, kDefaultMessagesPageSize);
  if (older && !older->empty()) {
    loaded_min_display_order_ = older->front().display_order;
    page_reading_.reset(); // a peer's page: chat.window without a local page's fields
    if (on_older_loaded_) {
      on_older_loaded_(); // the chat redraws its rows with them next (the caller's refresh)
    }
  }
}

} // namespace pbr
