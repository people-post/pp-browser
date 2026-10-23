#include "feature/conversations/ChatPresenter.h"

#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/SessionListText.h"
#include "foundation/platform/ILocalNotifier.h"
#include "lib/ai/StructuredTextParser.h"
#include "common/Utilities.h"

#include <algorithm>
#include <utility>

namespace pbr {

std::string ChatSessionKindName(const Thread& thread) {
  switch (thread.kind) {
  case ThreadKind::Ai:
    return "ai";
  case ThreadKind::Group:
    return "group";
  case ThreadKind::Direct:
    return thread.channel == ThreadChannel::E2e ? "private" : "public";
  }
  return "public";
}

ChatPresenter::ChatPresenter() {
  redirectLogger("ChatPresenter");
}

void ChatPresenter::BindFacade(ConversationsFacade* facade) {
  facade_ = facade;
  prefs_.reset();
}

void ChatPresenter::BindCallbacks(Callbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

void ChatPresenter::ResetForNewProfile() {
  prefs_.reset();
  showing_archived_ = false;
}

const std::unordered_map<std::string, ThreadLocalPrefs>& ChatPresenter::AllPrefs() {
  static const std::unordered_map<std::string, ThreadLocalPrefs> kUnavailable;
  if (!prefs_ && facade_) {
    auto listed = facade_->ListThreadLocalPrefs();
    if (!listed) {
      // Not cached: an empty cache would make every thread look default, and the next write would put
      // those defaults over the thread's stored row. Read again next time instead.
      return kUnavailable;
    }
    prefs_.emplace();
    for (ThreadLocalPrefs& prefs : *listed) {
      const std::string id = prefs.thread_id;
      prefs_->emplace(id, std::move(prefs));
    }
  }
  return prefs_ ? *prefs_ : kUnavailable;
}

std::optional<ThreadLocalPrefs> ChatPresenter::PrefsOf(const std::string& thread_id) {
  if (!facade_) {
    return std::nullopt;
  }
  const auto& all = AllPrefs();
  if (!prefs_) {
    auto one = facade_->GetThreadLocalPrefs(thread_id);
    if (!one) {
      return std::nullopt;
    }
    return *one;
  }
  if (const auto it = all.find(thread_id); it != all.end()) {
    return it->second;
  }
  ThreadLocalPrefs prefs;
  prefs.thread_id = thread_id;
  return prefs;
}

bool ChatPresenter::StorePrefs(const ThreadLocalPrefs& prefs) {
  if (!facade_ || !facade_->SetThreadLocalPrefs(prefs)) {
    return false;
  }
  if (prefs_) {
    (*prefs_)[prefs.thread_id] = prefs;
  }
  return true;
}

void ChatPresenter::UpdatePrefs(const std::string& thread_id, const std::function<void(ThreadLocalPrefs&)>& change) {
  std::optional<ThreadLocalPrefs> prefs = PrefsOf(thread_id);
  if (prefs) {
    change(*prefs);
  }
  if (!prefs || !StorePrefs(*prefs)) {
    if (callbacks_.prefs_failed) {
      callbacks_.prefs_failed();
    }
    Changed();
    return;
  }
  Refresh();
}

void ChatPresenter::Refresh() {
  ChatListScreen next;
  next.showing_archived = showing_archived_;
  auto threads = facade_ ? facade_->ListThreads() : Roe<std::vector<Thread>>(Error("Messaging not ready"));
  if (!threads) {
    list_ = std::move(next);
    Changed();
    return;
  }
  const auto& prefs = AllPrefs();
  const auto prefs_of = [&prefs](const Thread& thread) -> ThreadLocalPrefs {
    const auto it = prefs.find(thread.id);
    return it == prefs.end() ? ThreadLocalPrefs{} : it->second;
  };
  std::vector<Thread> sorted = std::move(*threads);
  std::stable_sort(sorted.begin(), sorted.end(), [&prefs_of](const Thread& a, const Thread& b) {
    return SessionSortsBefore(prefs_of(a).pinned_at, a.updated_at, prefs_of(b).pinned_at, b.updated_at);
  });
  for (const Thread& thread : sorted) {
    if (prefs_of(thread).archived) {
      ++next.archived_count;
    }
  }
  if (next.archived_count == 0) {
    showing_archived_ = false;  // the last archived chat left: back to the list
    next.showing_archived = false;
  }
  const std::string active_id = facade_->ActiveThreadId();
  const int64_t now_ms = util::NowUnixMs();
  for (const Thread& thread : sorted) {
    const ThreadLocalPrefs thread_prefs = prefs_of(thread);
    if (thread_prefs.archived != next.showing_archived) {
      continue;
    }
    ChatSessionRow row;
    row.id = thread.id;
    row.title = facade_->ResolveThreadLabel(thread).title;
    // The stored default AI title is English; the renderer shows it in the UI language.
    row.title_is_default_ai = thread.kind == ThreadKind::Ai && row.title == "New chat";
    row.preview = SessionPreviewLine(StructuredTextParser::PlainText(thread.preview));
    row.kind = ChatSessionKindName(thread);
    row.unread_count = thread.unread_count;
    row.unread_dot = SessionShowsUnreadDot(thread.unread_count, thread_prefs.marked_unread);
    row.pinned = thread_prefs.pinned_at != 0;
    row.muted = ThreadIsMuted(thread_prefs.muted_until, now_ms);
    row.date_label = SessionDateLabel(thread.updated_at, now_ms);
    row.active = thread.id == active_id;
    next.rows.push_back(std::move(row));
  }
  list_ = std::move(next);
  Changed();
}

void ChatPresenter::Changed() {
  if (callbacks_.on_changed) {
    callbacks_.on_changed();
  }
}

bool ChatPresenter::SelectThread(const std::string& thread_id) {
  if (!facade_ || !facade_->OpenThread(thread_id)) {
    return false;
  }
  // Opening a chat reads it: a dot set by "Mark as unread" goes with the unread count.
  if (std::optional<ThreadLocalPrefs> prefs = PrefsOf(thread_id); prefs && prefs->marked_unread) {
    prefs->marked_unread = false;
    (void)StorePrefs(*prefs);
  }
  ILocalNotifier::Instance().ClearForThread(thread_id);
  facade_->MaybeTailSync(thread_id);
  return true;
}

void ChatPresenter::ShowHome() {
  if (facade_) {
    facade_->ClearActiveThread();
  }
  Refresh();
}

void ChatPresenter::ToggleArchived() {
  showing_archived_ = !showing_archived_;
  Refresh();
}

void ChatPresenter::SetPinned(const std::string& thread_id, const bool pinned) {
  UpdatePrefs(thread_id, [pinned](ThreadLocalPrefs& prefs) { prefs.pinned_at = pinned ? util::NowUnixMs() : 0; });
}

void ChatPresenter::Mute(const std::string& thread_id, const SessionMuteChoice choice) {
  UpdatePrefs(thread_id,
              [choice](ThreadLocalPrefs& prefs) { prefs.muted_until = MutedUntilFor(choice, util::NowUnixMs()); });
}

void ChatPresenter::Unmute(const std::string& thread_id) {
  UpdatePrefs(thread_id, [](ThreadLocalPrefs& prefs) { prefs.muted_until = 0; });
}

void ChatPresenter::SetArchived(const std::string& thread_id, const bool archived) {
  UpdatePrefs(thread_id, [archived](ThreadLocalPrefs& prefs) { prefs.archived = archived; });
}

void ChatPresenter::MarkUnread(const std::string& thread_id) {
  // A mark, not a message count: the row shows a dot and keeps its place in the list.
  UpdatePrefs(thread_id, [](ThreadLocalPrefs& prefs) { prefs.marked_unread = true; });
}

void ChatPresenter::MarkRead(const std::string& thread_id) {
  if (facade_) {
    facade_->MarkThreadRead(thread_id);
  }
  UpdatePrefs(thread_id, [](ThreadLocalPrefs& prefs) { prefs.marked_unread = false; });
}

void ChatPresenter::ForgetThread(const std::string& thread_id) {
  if (prefs_) {
    prefs_->erase(thread_id);
  }
}

} // namespace pbr
