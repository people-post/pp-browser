#pragma once

#include "common/thread/ThreadRecordTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/** One searchable message of a chat: its text as the user reads it, folded for matching. In memory only. */
struct ThreadSearchEntry {
  std::string message_id;
  int64_t display_order = 0;
  std::string folded_text;
};

/** A message whose text contains the query. */
struct ThreadSearchHit {
  std::string message_id;
  int64_t display_order = 0;
  bool operator==(const ThreadSearchHit&) const = default;
};

/**
 * What the user reads in a message: an AI answer stored as a blocks document is its prose; a reply that quotes
 * another message is only the reply (the quoted text belongs to the other message); anything else is its text.
 */
std::string PlainTextOf(const ThreadMessage& message);

/** ASCII letters lowered, leading and trailing whitespace dropped; every other byte as it is (CJK matches as typed). */
std::string FoldForSearch(std::string_view text);

/**
 * The chat's searchable messages: text messages whose folded text is not empty. Recalled messages (Deleted),
 * attachments, calls, group events and cards are left out.
 */
std::vector<ThreadSearchEntry> BuildSearchIndex(const std::vector<ThreadMessage>& messages);

/** The entries whose text contains `query` (folded the same way), newest first. An empty query finds nothing. */
std::vector<ThreadSearchHit> FindHits(const std::vector<ThreadSearchEntry>& index, std::string_view query);

/**
 * The cursor in `new_hits` after the hits were found again: on the same message when it is still a hit, else 0.
 */
size_t RealignCurrent(const std::vector<ThreadSearchHit>& old_hits, size_t old_current,
                      const std::vector<ThreadSearchHit>& new_hits);

} // namespace pbr
