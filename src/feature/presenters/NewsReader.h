#pragma once

#include "common/thread/ThreadTypes.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/**
 * The reader's title for a news pick (one item: its "details" link, whose label is the generic "View details"): the
 * first line of the pick's text that has words, without Markdown; failing that, the host of `url`. Never the label.
 */
std::string NewsPickTitle(const std::string& pick_text, const std::string& url);

/** The news items an AI thread has read, by NormalizeNewsUrl. */
struct NewsReads {
  struct Digest {
    std::string message_id;
    std::string text;
    bool translated = false; // brief_AI translated it from the article's language: the reader says so
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

/**
 * What the reader shows for one item: its digest (and whether it is an AI translation); while it is being read, the
 * text streamed so far, or the spinner until the first of it; or the failure line.
 */
struct NewsReaderShows {
  const NewsReads::Digest* digest = nullptr; // points into the NewsReads passed in
  bool translated = false;                   // only with a digest: the line "AI translation of the original"
  bool reading = false;                      // this item is being read: Previous / Next stay off until it ends
  bool loading = false;                      // reading, and nothing of the text has come yet: the spinner line
  std::string_view partial;                  // reading: the text so far (points into `streamed`), never stored
  bool failed = false;
};
/** `streamed`: the text the running read of `reading_url` has streamed so far ("" while none has come). */
NewsReaderShows NewsReaderShowsFor(const NewsReads& reads, const std::string& item_url, const std::string& reading_url,
                                   std::string_view streamed = {});

/**
 * The text a news read has streamed so far (brief_AI's tokens, the whole text each time) and when the reader last drew
 * it. The reader redraws the growing text at most once per kRedrawInterval: each redraw converts the whole text again
 * and replaces the digest's elements, so a long digest coming token by token would otherwise lay the sheet out again
 * for every token. What a skipped redraw would have shown is drawn by the next due one (the chat's per-frame tick asks
 * DrawDue), and the end of the read shows the stored final text whatever was drawn. Memory only, never stored.
 */
struct NewsStreamText {
  static constexpr std::chrono::milliseconds kRedrawInterval{100}; // at most ten redraws a second

  std::string text;
  bool drawn = true; // the reader shows `text` (or does not show this read at all)
  std::optional<std::chrono::steady_clock::time_point> last_drawn;

  void Grow(std::string text_so_far) {
    if (text_so_far != text) {
      text = std::move(text_so_far);
      drawn = false;
    }
  }
  bool DrawDue(const std::chrono::steady_clock::time_point now) const {
    return !drawn && (!last_drawn || now - *last_drawn >= kRedrawInterval);
  }
  void Drawn(const std::chrono::steady_clock::time_point now) {
    drawn = true;
    last_drawn = now;
  }
};

/** Previous / Next under the reader: only for a list (two or more items), off at the ends and while a read runs. */
struct NewsNav {
  bool shown = false;
  bool can_prev = false;
  bool can_next = false;
};
/** `reading`: the item shown is being read (spinner or streamed text). */
NewsNav NewsNavFor(size_t index, size_t count, bool reading);

} // namespace pbr
