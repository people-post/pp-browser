#pragma once

#include "feature/presenters/ChatPresenter.h"

#include "common/thread/ThreadRecordTypes.h"
#include "lib/calls/CallSessionQueries.h"
#include "lib/people/PeerDisplayLabel.h"

#include <cstdint>
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pbr {

/**
 * The phone Home widgets (DESIGN-pp-ui 8.16, mockup 10-home-widgets; Kenneth 2026-11-04: like iPhone widgets, and
 * Home is never empty once the messages are read). Ids as stored in preferences.json (home_widgets_hidden).
 */
inline constexpr const char* kHomeWidgetFavorites = "favorites";
inline constexpr const char* kHomeWidgetRecentChats = "recent_chats";
inline constexpr const char* kHomeWidgetCalls = "calls";
inline constexpr const char* kHomeWidgetMentions = "mentions";
inline constexpr const char* kHomeWidgetNewPeople = "new_people";
inline constexpr const char* kHomeWidgetBrief = "brief";

/** Every widget once, in the default order. */
const std::vector<std::string>& HomeWidgetIds();
bool IsHomeWidget(const std::string& id);
/** A wide widget takes a whole row; a small one half of it (two side by side). */
bool HomeWidgetWide(const std::string& id);

/** Which widgets the user keeps on Home, and in which order (Edit Home; device-only). */
struct HomeWidgetLayout {
  /** Every known widget once, in Home's order. */
  std::vector<std::string> order;
  /** The widgets removed from Home (not drawn, not computed, not fetched). */
  std::set<std::string> hidden;

  bool Kept(const std::string& id) const { return hidden.count(id) == 0; }
  /** The kept widgets, in order (Edit Home's "On Home" card). */
  std::vector<std::string> OnHome() const;
  /** The removed ones, in order (Edit Home's "Add" card). */
  std::vector<std::string> Addable() const;

  bool operator==(const HomeWidgetLayout&) const = default;
};

/** From the stored lists: unknown and repeated ids are dropped; a widget the order lacks goes at its end. */
HomeWidgetLayout HomeWidgetLayoutFrom(const std::vector<std::string>& order, const std::vector<std::string>& hidden);
/** The removed widgets as stored (home_widgets_hidden), in order. */
std::vector<std::string> HiddenHomeWidgets(const HomeWidgetLayout& layout);
/** `id` off Home; an unknown id changes nothing. */
HomeWidgetLayout RemoveHomeWidget(HomeWidgetLayout layout, const std::string& id);
/** `id` back on Home, at its end; an unknown id changes nothing. */
HomeWidgetLayout AddHomeWidget(HomeWidgetLayout layout, const std::string& id);

/** A saved contact in the Favorites widget. */
struct HomeFavorite {
  std::string contact_id;
  /** The newest direct chat with them; empty when there is none yet (a tap starts one). */
  std::string thread_id;
  std::string title;
  std::string avatar_letter;
  int avatar_tone = 0;
  /** Local file of their cached photo; empty: the letter. */
  std::string icon_path;

  bool operator==(const HomeFavorite&) const = default;
};

/** The Mentions widget: unread @me in groups. */
struct HomeMentions {
  /** Unread messages that mention us, over every chat. */
  int count = 0;
  /** The most recently active chat with one (what a tap opens, at its oldest unread mention). */
  std::string thread_id;
  std::string title;
  std::string preview;
};

/** What Home shows about the chats list (computed from it; no storage of its own). */
struct HomeTodaySummary {
  /** The kHomeRecentRows most recently active chats, read or not (Recent chats). */
  std::vector<ChatSessionRow> recent;
  /** Unread chats with people who are neither saved contacts nor blocked (New people). */
  int new_contacts = 0;
  /** The first of those chats in list order (what a tap on New people opens); empty when none. */
  std::string first_stranger_thread_id;
  HomeMentions mentions;
};

inline constexpr size_t kHomeRecentRows = 3;

/**
 * A direct chat whose peer is not a saved contact. A blocked person is always a saved contact (the block is a trust
 * level on the contact), so blocked and saved people are both excluded; groups and the AI chat never count.
 */
bool IsStrangerChat(ThreadKind kind, const PeerDisplayLabel& label);

/** `rows`: the main (not archived) chats list. A widget the layout removed is left empty (not computed). */
HomeTodaySummary SummarizeHomeToday(const std::vector<ChatSessionRow>& rows, const HomeWidgetLayout& layout);

/** The starred contacts with no direct chat in `rows` (the caller looks them up to show them). */
std::vector<std::string> StarredWithoutChat(const std::vector<ChatSessionRow>& rows,
                                            const std::vector<std::string>& starred);

/**
 * The Favorites widget (常用联系人): up to five saved, unblocked contacts, the starred ones first in star order, then
 * the most recently active direct chats (FrequentContacts). `starred`: the star order; `starred_without_chat`: the
 * starred contacts StarredWithoutChat named, as looked up (blocked ones left out by the caller).
 */
std::vector<HomeFavorite> HomeFavorites(const std::vector<ChatSessionRow>& rows, const std::vector<std::string>& starred,
                                        const std::vector<HomeFavorite>& starred_without_chat);

/** The Calls widget. */
struct HomeCalls {
  /** Calls to us we missed since local midnight. */
  int missed_today = 0;
  /** The newest call, when it is from the last kHomeCallsRecentMs; none: the widget hides. */
  std::optional<call_queries::RecentCall> latest;
};

inline constexpr int64_t kHomeCallsRecentMs = 7LL * 24 * 60 * 60 * 1000;
/** How many calls Home reads (enough for a day's missed calls). */
inline constexpr size_t kHomeCallsRead = 50;

/** `calls`: newest first (call_queries::RecentCalls). */
HomeCalls SummarizeHomeCalls(const std::vector<call_queries::RecentCall>& calls, int64_t today_midnight_ms,
                             int64_t now_ms);

/** What each widget would show, for ShownHomeWidgets. */
struct HomeWidgetData {
  bool any_chat = false;
  bool calls = false;
  int mentions = 0;
  int new_people = 0;
  size_t brief_items = 0;
};
HomeWidgetData HomeWidgetDataOf(const HomeTodaySummary& summary, const HomeCalls& calls, size_t brief_items);

/**
 * The widgets drawn, in Home's order: the kept ones with something to show. Favorites always shows (with a hint when
 * empty); Recent chats once there is a chat; Calls, Mentions, New people and Today's briefs only with data.
 */
std::vector<std::string> ShownHomeWidgets(const HomeWidgetLayout& layout, const HomeWidgetData& data);

/** "11月3日 星期二" / "Tuesday, November 3" in the UI language. */
std::string HomeDateLabel(const std::tm& now);

} // namespace pbr
