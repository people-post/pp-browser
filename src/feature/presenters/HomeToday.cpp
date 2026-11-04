#include "feature/presenters/HomeToday.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/ui/NewChatList.h"

#include <algorithm>
#include <map>

namespace pbr {

const std::vector<std::string>& HomeWidgetIds() {
  // Mockup 10-home-widgets: the two small ones that pair up sit together.
  static const std::vector<std::string> ids = {kHomeWidgetFavorites, kHomeWidgetCalls,     kHomeWidgetMentions,
                                               kHomeWidgetRecentChats, kHomeWidgetBrief, kHomeWidgetNewPeople};
  return ids;
}

bool IsHomeWidget(const std::string& id) {
  const std::vector<std::string>& ids = HomeWidgetIds();
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool HomeWidgetWide(const std::string& id) {
  return id == kHomeWidgetFavorites || id == kHomeWidgetRecentChats || id == kHomeWidgetBrief;
}

std::vector<std::string> HomeWidgetLayout::OnHome() const {
  std::vector<std::string> ids;
  for (const std::string& id : order) {
    if (Kept(id)) {
      ids.push_back(id);
    }
  }
  return ids;
}

std::vector<std::string> HomeWidgetLayout::Addable() const {
  std::vector<std::string> ids;
  for (const std::string& id : order) {
    if (!Kept(id)) {
      ids.push_back(id);
    }
  }
  return ids;
}

HomeWidgetLayout HomeWidgetLayoutFrom(const std::vector<std::string>& order, const std::vector<std::string>& hidden) {
  HomeWidgetLayout layout;
  for (const std::string& id : order) {
    if (IsHomeWidget(id) && std::find(layout.order.begin(), layout.order.end(), id) == layout.order.end()) {
      layout.order.push_back(id);
    }
  }
  for (const std::string& id : HomeWidgetIds()) {
    if (std::find(layout.order.begin(), layout.order.end(), id) == layout.order.end()) {
      layout.order.push_back(id);
    }
  }
  for (const std::string& id : hidden) {
    if (IsHomeWidget(id)) {
      layout.hidden.insert(id);
    }
  }
  return layout;
}

std::vector<std::string> HiddenHomeWidgets(const HomeWidgetLayout& layout) {
  return layout.Addable();
}

HomeWidgetLayout RemoveHomeWidget(HomeWidgetLayout layout, const std::string& id) {
  if (IsHomeWidget(id)) {
    layout.hidden.insert(id);
  }
  return layout;
}

HomeWidgetLayout AddHomeWidget(HomeWidgetLayout layout, const std::string& id) {
  if (!IsHomeWidget(id) || layout.Kept(id)) {
    return layout;
  }
  layout.hidden.erase(id);
  // Back at the end of what Home shows (after the last kept widget), where Edit Home's list shows it.
  std::erase(layout.order, id);
  auto after = layout.order.begin();
  for (auto it = layout.order.begin(); it != layout.order.end(); ++it) {
    if (layout.Kept(*it)) {
      after = it + 1;
    }
  }
  layout.order.insert(after, id);
  return layout;
}

HomeWidgetLayout MoveHomeWidget(HomeWidgetLayout layout, const std::string& id, const size_t to) {
  std::vector<std::string> on_home = layout.OnHome();
  const auto it = std::find(on_home.begin(), on_home.end(), id);
  if (it == on_home.end()) {
    return layout;
  }
  on_home.erase(it);
  on_home.insert(on_home.begin() + static_cast<std::ptrdiff_t>(std::min(to, on_home.size())), id);
  const std::vector<std::string> removed = layout.Addable();
  on_home.insert(on_home.end(), removed.begin(), removed.end());
  layout.order = std::move(on_home);
  return layout;
}

bool IsStrangerChat(const ThreadKind kind, const PeerDisplayLabel& label) {
  return kind == ThreadKind::Direct && !label.contact_id && label.trust != PeerLabelTrust::Contact;
}

HomeTodaySummary SummarizeHomeToday(const std::vector<ChatSessionRow>& rows, const HomeWidgetLayout& layout,
                                    const std::optional<IThreadMentionsStore::Mention>& latest_mention,
                                    const int64_t now_ms) {
  HomeTodaySummary summary;
  if (layout.Kept(kHomeWidgetRecentChats)) {
    // The newest by activity, whatever is pinned at the top of the Chats list.
    std::vector<const ChatSessionRow*> by_time;
    for (const ChatSessionRow& row : rows) {
      by_time.push_back(&row);
    }
    std::stable_sort(by_time.begin(), by_time.end(),
                     [](const ChatSessionRow* a, const ChatSessionRow* b) { return a->updated_at > b->updated_at; });
    for (size_t i = 0; i < by_time.size() && i < kHomeRecentRows; ++i) {
      summary.recent.push_back(*by_time[i]);
    }
  }
  if (layout.Kept(kHomeWidgetNewPeople)) {
    for (const ChatSessionRow& row : rows) {
      if (row.unread_count > 0 && row.from_stranger) {
        if (summary.new_contacts == 0) {
          summary.first_stranger_thread_id = row.id;
        }
        ++summary.new_contacts;
      }
    }
  }
  if (layout.Kept(kHomeWidgetMentions)) {
    const ChatSessionRow* latest = nullptr;
    for (const ChatSessionRow& row : rows) {
      if (row.mention_count <= 0) {
        continue;
      }
      summary.mentions.count += row.mention_count;
      if (!latest || row.updated_at > latest->updated_at) {
        latest = &row;
      }
    }
    // The newest mention's chat, read or not, while it is recent: the widget stays after the chat is opened.
    if (latest_mention && now_ms - latest_mention->created_at <= kHomeMentionsRecentMs) {
      for (const ChatSessionRow& row : rows) {
        if (row.id == latest_mention->thread_id) {
          latest = &row;
          break;
        }
      }
    }
    if (latest) {
      summary.mentions.thread_id = latest->id;
      summary.mentions.title = latest->title;
      summary.mentions.preview = latest->preview;
    }
  }
  return summary;
}

std::vector<std::string> StarredWithoutChat(const std::vector<ChatSessionRow>& rows,
                                            const std::vector<std::string>& starred) {
  std::vector<std::string> missing;
  for (const std::string& id : starred) {
    const bool has_chat = std::any_of(rows.begin(), rows.end(), [&id](const ChatSessionRow& row) {
      return row.contact_id == id;
    });
    if (!has_chat) {
      missing.push_back(id);
    }
  }
  return missing;
}

std::vector<HomeFavorite> HomeFavorites(const std::vector<ChatSessionRow>& rows, const std::vector<std::string>& starred,
                                        const std::vector<HomeFavorite>& starred_without_chat) {
  // One candidate per contact, with its newest direct chat (the chats list is pinned first: look at the times).
  std::vector<HomeFavorite> candidates;
  std::vector<int64_t> newest;
  std::vector<NewChatContact> contacts;
  std::vector<NewChatDirectChat> chats;
  std::map<std::string, size_t> index_of;
  for (const ChatSessionRow& row : rows) {
    if (row.contact_id.empty()) {
      continue;
    }
    chats.push_back({row.contact_id, row.updated_at});
    if (const auto found = index_of.find(row.contact_id); found != index_of.end()) {
      const size_t i = found->second;
      contacts[i].blocked = contacts[i].blocked || row.blocked;
      if (row.updated_at > newest[i]) {
        newest[i] = row.updated_at;
        candidates[i].thread_id = row.id;
      }
      continue;
    }
    index_of[row.contact_id] = candidates.size();
    candidates.push_back({.contact_id = row.contact_id,
                          .thread_id = row.id,
                          .title = row.title,
                          .avatar_letter = row.avatar_letter,
                          .avatar_tone = row.avatar_tone,
                          .icon_path = row.icon_path});
    newest.push_back(row.updated_at);
    contacts.push_back({row.contact_id, row.title, row.blocked, StarRank(starred, row.contact_id)});
  }
  for (const HomeFavorite& favorite : starred_without_chat) {
    if (favorite.contact_id.empty() || index_of.count(favorite.contact_id) != 0) {
      continue;
    }
    index_of[favorite.contact_id] = candidates.size();
    candidates.push_back(favorite);
    candidates.back().thread_id.clear();
    contacts.push_back({favorite.contact_id, favorite.title, false, StarRank(starred, favorite.contact_id)});
  }
  std::vector<HomeFavorite> favorites;
  for (const size_t i : FrequentContacts(contacts, chats)) {
    favorites.push_back(candidates[i]);
  }
  return favorites;
}

HomeCalls SummarizeHomeCalls(const std::vector<call_queries::RecentCall>& calls, const int64_t today_midnight_ms,
                             const int64_t now_ms) {
  HomeCalls summary;
  for (const call_queries::RecentCall& call : calls) {
    if (call.missed && call.at_ms >= today_midnight_ms) {
      ++summary.missed_today;
    }
  }
  if (!calls.empty() && (now_ms - calls.front().at_ms <= kHomeCallsRecentMs || summary.missed_today > 0)) {
    summary.latest = calls.front();
  }
  return summary;
}

HomeWidgetData HomeWidgetDataOf(const HomeTodaySummary& summary, const HomeCalls& calls, const size_t brief_items) {
  return {.any_chat = !summary.recent.empty(),
          .calls = calls.latest.has_value(),
          .mentions = !summary.mentions.thread_id.empty(),
          .new_people = summary.new_contacts,
          .brief_items = brief_items};
}

std::vector<std::string> ShownHomeWidgets(const HomeWidgetLayout& layout, const HomeWidgetData& data) {
  std::vector<std::string> shown;
  for (const std::string& id : layout.OnHome()) {
    const bool has = id == kHomeWidgetFavorites || (id == kHomeWidgetRecentChats && data.any_chat) ||
                     (id == kHomeWidgetCalls && data.calls) || (id == kHomeWidgetMentions && data.mentions) ||
                     (id == kHomeWidgetNewPeople && data.new_people > 0) ||
                     (id == kHomeWidgetBrief && data.brief_items > 0);
    if (has) {
      shown.push_back(id);
    }
  }
  return shown;
}

std::string HomeDateLabel(const std::tm& now) {
  const int weekday = (now.tm_wday % 7 + 7) % 7;
  const int month = now.tm_mon + 1;
  return Tr("home.date",
            {{"weekday", Tr("home.weekday." + std::to_string(weekday))},
             {"month", Tr("home.month." + std::to_string(month))},
             {"day", std::to_string(now.tm_mday)}});
}

} // namespace pbr
