#include <stdexcept>
#include "feature/chat/ChatTranscriptScroller.h"

#include "base/i18n/LocalizationService.h"
#include "base/messaging/MessagingLimits.h"
#include "feature/ui/DataModelHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

void ChatTranscriptScroller::BindMessagingFacade(MessagingFacade* facade) {
  facade_ = facade;
}

ChatTranscriptScroller::ChatTranscriptScroller(Rml::Context*& context, View view, bool& messaging_ready)
    : context_(context), view_(view), messaging_ready_(messaging_ready) {}

void ChatTranscriptScroller::Reset() {
  pinned_to_bottom_ = true;
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
  last_messages_scroll_height_ = 0.f;
  unread_while_scrolled_ = 0;
  view_.show_jump_to_latest = false;
  view_.jump_to_latest_label = "";
}

Rml::Element* ChatTranscriptScroller::FindMessagesScrollElement() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return nullptr;
  }
  return context_->GetDocument(0)->GetElementById("chat-messages");
}

void ChatTranscriptScroller::UpdateJumpToLatestLabel() {
  if (unread_while_scrolled_ > 0) {
    view_.jump_to_latest_label =
        (std::to_string(unread_while_scrolled_) + " new · Latest").c_str();
  } else {
    view_.jump_to_latest_label = Tr("chat.jump_to_latest").c_str();
  }
}

void ChatTranscriptScroller::SetShowJumpToLatest(bool show) {
  if (view_.show_jump_to_latest == show && !show) {
    return;
  }
  view_.show_jump_to_latest = show;
  if (show) {
    UpdateJumpToLatestLabel();
  } else {
    unread_while_scrolled_ = 0;
    view_.jump_to_latest_label = "";
  }
  DataModelHost::Instance().Dirty("chat", "show_jump_to_latest");
  DataModelHost::Instance().Dirty("chat", "jump_to_latest_label");
}

void ChatTranscriptScroller::RequestScrollToLatest() {
  pinned_to_bottom_ = true;
  pending_scroll_to_bottom_ = true;
  pending_scroll_settle_frames_ = 0;
  pending_scroll_attempts_ = 0;
  settle_scroll_height_ = -1.f;
  last_messages_scroll_height_ = 0.f;
  SetShowJumpToLatest(false);
}

void ChatTranscriptScroller::ScrollMessagesToBottom() {
  Rml::Element* el = FindMessagesScrollElement();
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
  Rml::Element* el = FindMessagesScrollElement();
  if (!el) {
    return;
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

  const float scroll_height = el->GetScrollHeight();
  const float max_top = std::max(0.f, scroll_height - el->GetClientHeight());
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
  if (loading_older_local_ || !has_more_local_history_ || !messaging_ready_ ||
      pending_scroll_to_bottom_) {
    return;
  }
  Rml::Element* el = FindMessagesScrollElement();
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

  Rml::Element* el = FindMessagesScrollElement();
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
  has_more_local_history_ =
      facade_ &&
      facade_->HasLocalMessagesBefore(thread_id, *loaded_min_display_order_);

  view_.messages = facade_
                       ? facade_->BuildDisplayRows(thread_id, loaded_min_display_order_, loaded_max_display_order_)
                       : std::vector<MessageDisplayRow>{};
  if (TrimDomWindow(view_.messages, /*pinned_to_bottom=*/false, loaded_min_display_order_,
                    loaded_max_display_order_, has_more_local_history_)) {
    pinned_to_bottom_ = false;
    SetShowJumpToLatest(true);
    // Rebuild with new max bound so store sync stays coherent.
    view_.messages = facade_
                         ? facade_->BuildDisplayRows(thread_id, loaded_min_display_order_, loaded_max_display_order_)
                         : std::vector<MessageDisplayRow>{};
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
  Rml::Element* el = FindMessagesScrollElement();
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
  }
  MaybeLoadOlderLocalHistory();
}

void ChatTranscriptScroller::OnJumpToLatest() {
  loaded_max_display_order_.reset();
  loaded_min_display_order_.reset();
  if (facade_) {
    const std::string thread_id = facade_->ActiveThreadId();
    if (!thread_id.empty()) {
      view_.messages = facade_->BuildDisplayRows(thread_id, std::nullopt, std::nullopt);
      view_.has_turns = !view_.messages.empty();
      if (!view_.messages.empty()) {
        loaded_min_display_order_ = view_.messages.front().display_order;
        has_more_local_history_ =
            facade_->HasLocalMessagesBefore(thread_id, view_.messages.front().display_order);
      }
      if (dirty_turns_) {
        dirty_turns_();
      }
    }
  }
  RequestScrollToLatest();
  ScrollMessagesToBottom();
}

void ChatTranscriptScroller::OnShellRemounted() {
  if (pinned_to_bottom_ && view_.has_turns) {
    pending_scroll_to_bottom_ = true;
    pending_scroll_settle_frames_ = 0;
    pending_scroll_attempts_ = 0;
    settle_scroll_height_ = -1.f;
    last_messages_scroll_height_ = 0.f;
  }
  ApplyPolicy();
}

bool ChatTranscriptScroller::BeginDisplaySync(const std::string& thread_id) {
  const bool thread_changed = thread_id != scroll_thread_id_;
  if (thread_changed) {
    scroll_thread_id_ = thread_id;
    loaded_min_display_order_.reset();
    loaded_max_display_order_.reset();
    unread_while_scrolled_ = 0;
    RequestScrollToLatest();
  }
  return thread_changed;
}

void ChatTranscriptScroller::EndDisplaySync(bool thread_changed, const std::string& prev_tail_id,
                                            size_t prev_count) {
  if (!view_.messages.empty()) {
    if (TrimDomWindow(view_.messages, pinned_to_bottom_, loaded_min_display_order_, loaded_max_display_order_,
                      has_more_local_history_)) {
      if (pinned_to_bottom_ && facade_) {
        const std::string thread_id = facade_->ActiveThreadId();
        view_.messages =
            facade_->BuildDisplayRows(thread_id, loaded_min_display_order_, loaded_max_display_order_);
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
          facade_ &&
          facade_->HasLocalMessagesBefore(thread_id, view_.messages.front().display_order);
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
                                           std::optional<int64_t>& loaded_min,
                                           std::optional<int64_t>& loaded_max, bool& has_more_local_history) {
  if (messages.size() <= kMaxMessagesDomWindow) {
    return false;
  }
  if (pinned_to_bottom) {
    const size_t drop = messages.size() - kMaxMessagesDomWindow;
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
  Rml::Element* el = FindMessagesScrollElement();
  if (el && !pinned_to_bottom_) {
    pending_scroll_height_before_ = el->GetScrollHeight();
    pending_scroll_top_before_ = el->GetScrollTop();
  }
}

void ChatTranscriptScroller::ExpandLoadedMinFromOlderPage(const std::string& thread_id,
                                                          int64_t before_display_order) {
  auto older =
      facade_ ? facade_->GetMessagesPage(thread_id, before_display_order, kDefaultMessagesPageSize)
                                    : Roe<std::vector<ThreadMessage>>::error(Error("chat port unavailable"));
  if (older && !older->empty()) {
    loaded_min_display_order_ = older->front().display_order;
  }
}

} // namespace pbr
