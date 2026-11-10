#pragma once

#include "lib/ui/NameIndex.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr {

/** One contact as the phone "New chat" page sees it. */
struct NewChatContact {
  std::string id;
  std::string name;
  bool blocked = false;
  /** Starred as a favourite (常用联系人): its place in the star order (0 first), -1 when not starred. */
  int star_rank = -1;
};

/** A direct chat: the contact it is with and when it was last active. */
struct NewChatDirectChat {
  std::string contact_id;
  int64_t updated_at = 0;
};

/** A letter and the contacts under it (indexes into the contact list given to BuildNewChatList). */
struct NewChatSection {
  std::string letter;
  std::vector<size_t> contacts;
};

struct NewChatList {
  /** Indexes of the starred contacts (in star order), then the most recently chatted-with ones, newest first. */
  std::vector<size_t> frequent;
  /** A-Z, then "#". */
  std::vector<NewChatSection> sections;
};

inline constexpr size_t kNewChatFrequentMax = 5;

/** A contact's place in the stored star order (ProfilePreferences::starred_contacts); -1 when not starred. */
inline int StarRank(const std::vector<std::string>& starred, const std::string& contact_id) {
  const auto it = std::find(starred.begin(), starred.end(), contact_id);
  return it == starred.end() ? -1 : static_cast<int>(it - starred.begin());
}

/** The star order with `contact_id` starred (added last) or, when it was starred, unstarred. */
inline std::vector<std::string> ToggleStarred(std::vector<std::string> starred, const std::string& contact_id) {
  const auto it = std::find(starred.begin(), starred.end(), contact_id);
  if (it != starred.end()) {
    starred.erase(it);
  } else if (!contact_id.empty()) {
    starred.push_back(contact_id);
  }
  return starred;
}

namespace new_chat_detail {

inline char Lower(const char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string LowerAscii(std::string text) {
  for (char& c : text) {
    c = Lower(c);
  }
  return text;
}

} // namespace new_chat_detail

/** The section letter of a name: A-Z (a Chinese name by its pinyin initial), else "#" (NameIndexLetter). */
inline std::string NewChatLetter(const std::string& name) {
  return std::string(1, NameIndexLetter(name));
}

/**
 * The frequent contacts (常用联系人; the New chat page, Home's Favourites widget): up to kNewChatFrequentMax, the
 * starred ones first in star order, then those with a direct chat, most recently active first (no activity time, no
 * place). Blocked contacts never show. Indexes into `contacts`.
 */
inline std::vector<size_t> FrequentContacts(const std::vector<NewChatContact>& contacts,
                                            const std::vector<NewChatDirectChat>& chats) {
  std::unordered_map<std::string, int64_t> last_active;
  for (const NewChatDirectChat& chat : chats) {
    if (chat.updated_at > 0) {
      int64_t& at = last_active[chat.contact_id];
      at = std::max(at, chat.updated_at);
    }
  }
  std::vector<size_t> starred;
  std::vector<size_t> recent;
  for (size_t i = 0; i < contacts.size(); ++i) {
    if (contacts[i].blocked) {
      continue;
    }
    if (contacts[i].star_rank >= 0) {
      starred.push_back(i);
    } else if (last_active.count(contacts[i].id) != 0) {
      recent.push_back(i);
    }
  }
  std::sort(starred.begin(), starred.end(), [&](const size_t a, const size_t b) {
    return contacts[a].star_rank != contacts[b].star_rank ? contacts[a].star_rank < contacts[b].star_rank
                                                          : contacts[a].id < contacts[b].id;
  });
  std::sort(recent.begin(), recent.end(), [&](const size_t a, const size_t b) {
    const int64_t ta = last_active[contacts[a].id];
    const int64_t tb = last_active[contacts[b].id];
    return ta != tb ? ta > tb : contacts[a].id < contacts[b].id;
  });
  starred.insert(starred.end(), recent.begin(), recent.end());
  if (starred.size() > kNewChatFrequentMax) {
    starred.resize(kNewChatFrequentMax);
  }
  return starred;
}

/**
 * The page's rows. Blocked contacts are never listed. Without a query: "frequent" is FrequentContacts, and every
 * contact is also in the lettered sections, sorted by name (A-Z case-blind, Chinese by pinyin initial, then "#"; NameIndexLess).
 * With a query: no frequent group; the sections hold the contacts whose name contains it (case-blind) or whose pinyin initials start with it.
 */
inline NewChatList BuildNewChatList(const std::vector<NewChatContact>& contacts,
                                    const std::vector<NewChatDirectChat>& chats, const std::string& query = {}) {
  NewChatList out;
  const std::string needle = new_chat_detail::LowerAscii(query);

  std::vector<size_t> order;
  for (size_t i = 0; i < contacts.size(); ++i) {
    if (!contacts[i].blocked) {
      order.push_back(i);
    }
  }

  if (needle.empty()) {
    out.frequent = FrequentContacts(contacts, chats);
  } else {
    order.erase(std::remove_if(order.begin(), order.end(),
                               [&](const size_t i) { return !TextMatchesQuery(contacts[i].name, query); }),
                order.end());
  }

  std::sort(order.begin(), order.end(), [&](const size_t a, const size_t b) {
    if (NameIndexLess(contacts[a].name, contacts[b].name)) {
      return true;
    }
    if (NameIndexLess(contacts[b].name, contacts[a].name)) {
      return false;
    }
    return contacts[a].id < contacts[b].id;
  });

  for (const size_t i : order) {
    const std::string letter = NewChatLetter(contacts[i].name);
    if (out.sections.empty() || out.sections.back().letter != letter) {
      out.sections.push_back({letter, {}});
    }
    out.sections.back().contacts.push_back(i);
  }
  return out;
}

} // namespace pbr
