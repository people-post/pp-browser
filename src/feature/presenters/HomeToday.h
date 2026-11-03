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
};

inline constexpr size_t kHomeUnreadRows = 3;

/**
 * A direct chat whose peer is not a saved contact. A blocked person is always a saved contact (the block is a trust
 * level on the contact), so blocked and saved people are both excluded; groups and the AI chat never count.
 */
bool IsStrangerChat(ThreadKind kind, const PeerDisplayLabel& label);

/** `rows`: the main (not archived) chats list. */
HomeTodaySummary SummarizeHomeToday(const std::vector<ChatSessionRow>& rows);

/** "11月3日 星期二" / "Tuesday, November 3" in the UI language. */
std::string HomeDateLabel(const std::tm& now);

} // namespace pbr
