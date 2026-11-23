#pragma once

#include "common/thread/ThreadTypes.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * The news reader (DEV/PLAN-news-inline-digest.md): a tap on a news item of an AI answer — a `pp:news` link of a
 * headline list, or the "details" link of a single news pick — opens a sheet that shows a faithful digest of the
 * article, read by brief_AI on demand (no prefetch). The request and the digest are stored in the AI thread as hidden
 * messages (kNewsRequestGeneration, kNewsDigestGeneration / kNewsFailedGeneration): an item is read when a digest
 * answers its request. Pure decisions here; the chat screen wires them.
 */

/** One news item of an answer: the title the reader shows on top (the link's text) and the URL it reads. */
struct NewsItem {
  std::string url;
  std::string title;
};

/**
 * The URL two news items are compared by: every `utm_*` query parameter and the `#fragment` dropped, nothing else; a
 * hash route (`#/…`, `#!…`) is kept, as it names the page.
 */
std::string NormalizeNewsUrl(std::string_view url);

/**
 * What brief_AI is sent to read `url` (contract section 15): line 1 the URL only, line 2 the fixed sentence of the
 * UI language, which also sets the digest's language. Never the title: it may hold another URL or analysis words.
 */
std::string NewsReadRequestText(const std::string& url, const std::string& ui_language);

/** The news items an AI thread has read, by NormalizeNewsUrl. */
struct NewsReads {
  struct Digest {
    std::string message_id;
    std::string text;
  };
  std::map<std::string, Digest> digests;

  /** The digest stored for `url` (any form of it), or null: not read, or its read failed. */
  const Digest* Find(const std::string& url) const;
  bool IsRead(const std::string& url) const { return Find(url) != nullptr; }
};

/**
 * Pairs each news request of `oldest_first` (one thread's messages) with the hidden answer after it. A digest makes
 * the item read; a "could not be read" answer, or no answer at all (an error, a stop, the app closed mid-read),
 * leaves it unread, so a tap reads it again.
 */
NewsReads ScanNewsReads(const std::vector<ThreadMessage>& oldest_first);

/** The newest message of `oldest_first` the chat shows (a news read's hidden messages skipped); null: none. */
const ThreadMessage* NewestShownMessage(const std::vector<ThreadMessage>& oldest_first);

/** What a tap on a news item does. */
enum class NewsTap {
  ShowStored,  // its digest is stored: shown at once, no request
  ShowReading, // it is being read now (the sheet was closed meanwhile): the sheet shows it loading again
  StartRead,   // read it now
  Wait,        // the conversation is answering something else: only the pill "wait for this answer to finish"
};

/** `stored`: a digest is stored for it; `reading_this`: it is the item being read; `busy`: a turn runs in the chat. */
NewsTap DecideNewsTap(bool stored, bool reading_this, bool busy);
/** DecideNewsTap for the item at `item_url`: `reading_url` is the URL the chat's running read reads ("" when none). */
NewsTap DecideNewsTapFor(const NewsReads& reads, const std::string& item_url, const std::string& reading_url,
                         bool busy);

/** Previous / Next under the reader: only for a list (two or more items), off at the ends and while a read runs. */
struct NewsNav {
  bool shown = false;
  bool can_prev = false;
  bool can_next = false;
};
NewsNav NewsNavFor(size_t index, size_t count, bool loading);

} // namespace pbr
