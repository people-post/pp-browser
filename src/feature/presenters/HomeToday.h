#pragma once

#include "feature/presenters/ChatPresenter.h"

#include "common/thread/ThreadRecordTypes.h"
#include "lib/calls/CallSessionQueries.h"
#include "lib/messaging/IThreadMentionsStore.h"
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
/**
 * `id` moved to place `to` among the widgets on Home (OnHome; past the end: last); the removed ones keep their order
 * after them. A widget not on Home changes nothing.
 */
HomeWidgetLayout MoveHomeWidget(HomeWidgetLayout layout, const std::string& id, size_t to);

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
  /** Not starred: someone they chat with lately, filling the first row (shown marked as a suggestion). */
  bool suggested = false;

  bool operator==(const HomeFavorite&) const = default;
};

/** The Mentions widget: @me in groups, shown while the newest is recent (like Calls), read or not. */
struct HomeMentions {
  /** Unread messages that mention us, over every chat (the big number; 0: "Recent mention" instead). */
  int count = 0;
  /** The chat of the newest mention (what a tap opens, at its oldest unread mention if any); empty: hidden. */
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

/** How long the Mentions widget keeps showing the newest mention once it is read. */
inline constexpr int64_t kHomeMentionsRecentMs = 7LL * 24 * 60 * 60 * 1000;

/**
 * `rows`: the main (not archived) chats list. A widget the layout removed is left empty (not computed).
 * `latest_mention`: the newest mention, read or not (IThreadMentionsStore::Latest); shown while it is at most
 * kHomeMentionsRecentMs older than `now_ms` and its chat is in `rows`.
 */
HomeTodaySummary SummarizeHomeToday(const std::vector<ChatSessionRow>& rows, const HomeWidgetLayout& layout,
                                    const std::optional<IThreadMentionsStore::Mention>& latest_mention = std::nullopt,
                                    int64_t now_ms = 0);

/** The starred contacts with no direct chat in `rows` (the caller looks them up to show them). */
std::vector<std::string> StarredWithoutChat(const std::vector<ChatSessionRow>& rows,
                                            const std::vector<std::string>& starred);

/** The most people the Favorites row holds (three screens of four). */
inline constexpr size_t kHomeFavoritesMax = 12;
/** Fewer starred than this: the row is filled up to it with suggestions (one full row of four). */
inline constexpr size_t kHomeFavoritesFillTo = 4;

/**
 * The Favorites widget (关注联系人, Kenneth 2026-11-05 原话 75-77, 89): the starred contacts in star order, up to
 * kHomeFavoritesMax; a blocked one never shows. With fewer than kHomeFavoritesFillTo starred, the row is filled up to
 * it with the saved contacts chatted with most recently (`suggested`); with none of those either the widget shows its
 * hint. Each opens its newest direct chat in `rows`. `starred`: the star order; `starred_without_chat`: the starred
 * contacts StarredWithoutChat named, as looked up (blocked ones left out by the caller).
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
  /** A mention to show (unread, or read within kHomeMentionsRecentMs). */
  bool mentions = false;
  int new_people = 0;
  size_t brief_items = 0;
};
HomeWidgetData HomeWidgetDataOf(const HomeTodaySummary& summary, const HomeCalls& calls, size_t brief_items);

/**
 * The widgets drawn, in Home's order: the kept ones with something to show. Favorites always shows (with a hint when
 * empty), Mentions and New people always show (0 when none); Recent chats once there is a chat; Calls and Today's
 * briefs only with data.
 */
std::vector<std::string> ShownHomeWidgets(const HomeWidgetLayout& layout, const HomeWidgetData& data);

/** "11月3日 星期二" / "Tuesday, November 3" in the UI language. */
std::string HomeDateLabel(const std::tm& now);

} // namespace pbr
