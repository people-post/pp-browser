#include "feature/presenters/HomeToday.h"

#include "foundation/i18n/LocalizationService.h"

namespace pbr {

bool IsStrangerChat(const ThreadKind kind, const PeerDisplayLabel& label) {
  return kind == ThreadKind::Direct && !label.contact_id && label.trust != PeerLabelTrust::Contact;
}

HomeTodaySummary SummarizeHomeToday(const std::vector<ChatSessionRow>& rows) {
  HomeTodaySummary summary;
  for (const ChatSessionRow& row : rows) {
    if (row.unread_count <= 0) {
      continue;
    }
    summary.unread_total += row.unread_count;
    if (row.from_stranger) {
      if (summary.new_contacts == 0) {
        summary.first_stranger_thread_id = row.id;
      }
      ++summary.new_contacts;
    }
    if (summary.unread.size() < kHomeUnreadRows) {
      summary.unread.push_back(row);
    }
  }
  return summary;
}

HomeWidgetChoices HomeWidgetChoicesFromHidden(const std::vector<std::string>& hidden) {
  HomeWidgetChoices choices;
  for (const std::string& id : hidden) {
    if (id == kHomeWidgetMessages) {
      choices.messages = false;
    } else if (id == kHomeWidgetNewPeople) {
      choices.new_contacts = false;
    } else if (id == kHomeWidgetBrief) {
      choices.brief = false;
    }
  }
  return choices;
}

std::vector<std::string> HiddenHomeWidgets(const HomeWidgetChoices& choices) {
  std::vector<std::string> hidden;
  if (!choices.messages) {
    hidden.emplace_back(kHomeWidgetMessages);
  }
  if (!choices.new_contacts) {
    hidden.emplace_back(kHomeWidgetNewPeople);
  }
  if (!choices.brief) {
    hidden.emplace_back(kHomeWidgetBrief);
  }
  return hidden;
}

HomeWidgetChoices ToggleHomeWidget(HomeWidgetChoices choices, const std::string& id) {
  if (id == kHomeWidgetMessages) {
    choices.messages = !choices.messages;
  } else if (id == kHomeWidgetNewPeople) {
    choices.new_contacts = !choices.new_contacts;
  } else if (id == kHomeWidgetBrief) {
    choices.brief = !choices.brief;
  }
  return choices;
}

HomeWidgetsShown ShownHomeWidgets(const HomeTodaySummary& summary, const size_t brief_items,
                                  const HomeWidgetChoices& choices) {
  HomeWidgetsShown shown;
  shown.messages = choices.messages && !summary.unread.empty();
  shown.new_contacts = choices.new_contacts && summary.new_contacts > 0;
  shown.brief = choices.brief && brief_items > 0;
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
