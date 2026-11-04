#pragma once

#include "feature/presenters/ChatPresenter.h"

#include "common/thread/ThreadRecordTypes.h"
#include "lib/people/PeerDisplayLabel.h"

#include <ctime>
#include <string>
#include <vector>

namespace pbr {

/** What the phone's Today page shows about the chats list (DESIGN-pp-ui 8.16); no storage of its own. */
struct HomeTodaySummary {
  /** The first kHomeUnreadRows chats with unread messages, in list order. */
  std::vector<ChatSessionRow> unread;
  /** Unread messages over all chats (what the Chats tab badge counts). */
  int unread_total = 0;
  /** Unread chats with people who are neither saved contacts nor blocked. */
  int new_contacts = 0;
  /** The first of those chats in list order (what a tap on the New people widget opens); empty when none. */
  std::string first_stranger_thread_id;
};

/** Which Today widgets are drawn: each only when it has something to show (Kenneth 2026-11-03). */
struct HomeWidgetsShown {
  bool messages = false;
  bool new_contacts = false;
  bool brief = false;
};

inline constexpr size_t kHomeUnreadRows = 3;

/**
 * A direct chat whose peer is not a saved contact. A blocked person is always a saved contact (the block is a trust
 * level on the contact), so blocked and saved people are both excluded; groups and the AI chat never count.
 */
bool IsStrangerChat(ThreadKind kind, const PeerDisplayLabel& label);

/** `rows`: the main (not archived) chats list. */
HomeTodaySummary SummarizeHomeToday(const std::vector<ChatSessionRow>& rows);

/** Which widgets the user keeps on Home (Edit Home; all on by default). Stored as the ids switched off. */
struct HomeWidgetChoices {
  bool messages = true;
  bool new_contacts = true;
  bool brief = true;

  bool operator==(const HomeWidgetChoices&) const = default;
};

/** Widget ids in ProfilePreferences::home_widgets_hidden and in the Edit Home switches. */
inline constexpr const char* kHomeWidgetMessages = "messages";
inline constexpr const char* kHomeWidgetNewPeople = "new_people";
inline constexpr const char* kHomeWidgetBrief = "brief";

/** Unknown ids are ignored. */
HomeWidgetChoices HomeWidgetChoicesFromHidden(const std::vector<std::string>& hidden);
std::vector<std::string> HiddenHomeWidgets(const HomeWidgetChoices& choices);
/** `id` switched the other way; an unknown id changes nothing. */
HomeWidgetChoices ToggleHomeWidget(HomeWidgetChoices choices, const std::string& id);

/**
 * `brief_items`: how many headlines the Today's briefs widget would list. A widget shows when the user keeps it and
 * it has something to show.
 */
HomeWidgetsShown ShownHomeWidgets(const HomeTodaySummary& summary, size_t brief_items,
                                  const HomeWidgetChoices& choices = {});

/** "11月3日 星期二" / "Tuesday, November 3" in the UI language. */
std::string HomeDateLabel(const std::tm& now);

} // namespace pbr
