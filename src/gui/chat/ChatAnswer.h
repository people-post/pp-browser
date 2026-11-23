#pragma once

#include "lib/ai/BriefAiClient.h"
#include "lib/ai/MarkdownToRml.h"
#include "lib/ai/StructuredTextParser.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

constexpr std::chrono::milliseconds kStreamRenderInterval{100};
/** Bytes kept free in a stored answer for the trailing details link (label plus URL). */
constexpr size_t kDetailsLinkHeadroomBytes = 4096;

/** Pure rules for the streamed Markdown answer bubble (no RmlUi). */

/** True when a pending delta may be rendered now: first render, or `kStreamRenderInterval` since the last. */
inline bool ShouldRenderStreamDelta(const std::optional<std::chrono::steady_clock::time_point>& last_render,
                                    const std::chrono::steady_clock::time_point now) {
  return !last_render || now - *last_render >= kStreamRenderInterval;
}

inline bool IsHttpsUrl(const std::string& url) {
  return url.size() > 8 && url.compare(0, 8, "https://") == 0;
}

/** An https URL with no control characters: the only kind handed to the OS to open (PB-072). */
inline bool IsOpenableUrl(const std::string& url) {
  if (!IsHttpsUrl(url)) {
    return false;
  }
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

/** Host of an https URL for display; userinfo and port are dropped so "a.com@evil.com" shows evil.com.
 *  A backslash ends the host too, as browsers treat it like a slash (PB-072). */
inline std::string UrlHost(const std::string& url) {
  if (!IsHttpsUrl(url)) {
    return {};
  }
  std::string_view rest(url);
  rest.remove_prefix(8);
  rest = rest.substr(0, rest.find_first_of("/?#\\"));
  if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
    rest.remove_prefix(at + 1);
  }
  if (!rest.empty() && rest.front() == '[') {
    if (const size_t close = rest.find(']'); close != std::string_view::npos) {
      rest = rest.substr(0, close + 1);
    }
  } else {
    rest = rest.substr(0, rest.find(':'));
  }
  return std::string(rest);
}

struct ChatAnswerRml {
  std::string rml;
  std::vector<MarkdownLink> links; // indexed by open_chat_link('__ENTRY__', N)
};

/**
 * The answer text to store and render: the Markdown plus one trailing "details" link to the first
 * https source. Keeping the link in the text itself is what lets it survive a restart (a separate
 * sources list was only held in memory, so its links went dead). Unchanged when there is no https
 * source or the answer already links to it. `news_pick` (done.answer_kind "news_pick"): the link is marked as a
 * news item (title `pp:news`), so a tap opens the reader instead of the browser.
 */
inline std::string WithDetailsLink(std::string_view markdown, const std::vector<BriefAiSource>& sources,
                                   const std::string& label, const bool news_pick = false) {
  std::string out(markdown);
  const auto source =
      std::find_if(sources.begin(), sources.end(), [](const BriefAiSource& s) { return IsHttpsUrl(s.url); });
  if (source == sources.end()) {
    return out;
  }
  // Characters that would end or break a Markdown link destination.
  std::string dest;
  for (const char c : source->url) {
    switch (c) {
      case '(':
        dest += "%28";
        break;
      case ')':
        dest += "%29";
        break;
      case ' ':
        dest += "%20";
        break;
      case '<':
        dest += "%3C";
        break;
      case '>':
        dest += "%3E";
        break;
      case '\\':
        dest += "%5C";
        break;
      default:
        dest += c;
    }
  }
  const std::vector<MarkdownLink> existing = MarkdownToRml(markdown).links;
  if (std::any_of(existing.begin(), existing.end(), [&dest](const MarkdownLink& link) { return link.url == dest; })) {
    return out;
  }
  out += "\n\n[" + label + "](" + dest + (news_pick ? " \"" + std::string(kNewsLinkTitle) + "\"" : "") + ")";
  return out;
}

/**
 * Markdown answer as bubble markup; `links` is indexed by open_chat_link('__ENTRY__', N). `news_links`: the AI
 * assistant's own answer in an AI conversation, the only place a `pp:news` title is honoured.
 */
inline ChatAnswerRml BuildMarkdownAnswer(std::string_view markdown, const bool news_links = false) {
  MarkdownRml body = MarkdownToRml(markdown, MarkdownOptions{.news_links = news_links});
  return ChatAnswerRml{std::move(body.rml), std::move(body.links)};
}

/** The link a click may act on: in range and https. Everything else is dropped. */
inline std::optional<MarkdownLink> ResolveChatLink(const std::vector<MarkdownLink>& links, const int index) {
  if (index < 0 || static_cast<size_t>(index) >= links.size() || !IsHttpsUrl(links[static_cast<size_t>(index)].url)) {
    return std::nullopt;
  }
  return links[static_cast<size_t>(index)];
}

/** Links of a stored message after a restart, from its own Markdown text (`news_links` as BuildMarkdownAnswer). */
inline std::vector<MarkdownLink> RecoverChatLinks(std::string_view stored_markdown, const bool news_links = false) {
  return MarkdownToRml(stored_markdown, MarkdownOptions{.news_links = news_links}).links;
}

/** The news items among `links`, in the order they appear in the message. */
inline std::vector<MarkdownLink> NewsItemsOf(const std::vector<MarkdownLink>& links) {
  std::vector<MarkdownLink> items;
  for (const MarkdownLink& link : links) {
    if (link.news) {
      items.push_back(link);
    }
  }
  std::sort(items.begin(), items.end(),
            [](const MarkdownLink& a, const MarkdownLink& b) { return a.news_ordinal < b.news_ordinal; });
  return items;
}

/**
 * Greys the news items already read: adds kNewsReadClass to the span of each news link whose URL `is_read`.
 * `rml` is a bubble as MarkdownToRml wrote it (its `__ENTRY__` may already be the message id); `links` its table.
 */
template <typename IsRead>
std::string MarkNewsLinksRead(std::string rml, const std::vector<MarkdownLink>& links, const IsRead& is_read) {
  const std::string open = "<span class=\"chat-link " + std::string(kNewsLinkClass) + "\"";
  const std::string click = " data-event-click=\"open_chat_link('";
  size_t at = 0;
  while ((at = rml.find(open, at)) != std::string::npos) {
    const size_t args = at + open.size();
    if (rml.compare(args, click.size(), click) != 0) {
      at = args;
      continue;
    }
    const size_t comma = rml.find("', ", args + click.size());
    const size_t close = comma == std::string::npos ? comma : rml.find(')', comma);
    if (close == std::string::npos) {
      break;
    }
    const std::string digits = rml.substr(comma + 3, close - comma - 3);
    const bool number = !digits.empty() && digits.size() < 9 &&
                        std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
    const size_t index = number ? static_cast<size_t>(std::stoul(digits)) : links.size();
    if (index < links.size() && links[index].news && is_read(links[index].url)) {
      rml.insert(args - 1, " " + std::string(kNewsReadClass));
    }
    at = close;
  }
  return rml;
}

} // namespace pbr
