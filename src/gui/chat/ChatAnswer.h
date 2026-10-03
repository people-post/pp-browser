#pragma once

#include "domain/ai/BriefAiClient.h"
#include "domain/ai/MarkdownToRml.h"
#include "domain/ai/StructuredTextParser.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

constexpr std::chrono::milliseconds kStreamRenderInterval{100};

/** Pure rules for the streamed Markdown answer bubble (no RmlUi). */

/** True when a pending delta may be rendered now: first render, or `kStreamRenderInterval` since the last. */
inline bool ShouldRenderStreamDelta(const std::optional<std::chrono::steady_clock::time_point>& last_render,
                                    const std::chrono::steady_clock::time_point now) {
  return !last_render || now - *last_render >= kStreamRenderInterval;
}

inline bool IsHttpsUrl(const std::string& url) {
  return url.size() > 8 && url.compare(0, 8, "https://") == 0;
}

/** Host of an https URL for display; userinfo and port are dropped so "a.com@evil.com" shows evil.com. */
inline std::string UrlHost(const std::string& url) {
  if (!IsHttpsUrl(url)) {
    return {};
  }
  std::string_view rest(url);
  rest.remove_prefix(8);
  rest = rest.substr(0, rest.find_first_of("/?#"));
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
  std::vector<std::string> links; // indexed by open_chat_link('__ENTRY__', N)
};

/**
 * The answer text to store and render: the Markdown plus one trailing "details" link to the first
 * https source. Keeping the link in the text itself is what lets it survive a restart (a separate
 * sources list was only held in memory, so its links went dead). Unchanged when there is no https
 * source or the answer already links to it.
 */
inline std::string WithDetailsLink(std::string_view markdown, const std::vector<BriefAiSource>& sources,
                                   const std::string& label) {
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
  const std::vector<std::string> existing = MarkdownToRml(markdown).links;
  if (std::find(existing.begin(), existing.end(), dest) != existing.end()) {
    return out;
  }
  out += "\n\n[" + label + "](" + dest + ")";
  return out;
}

/** Markdown answer as bubble markup; `links` is indexed by open_chat_link('__ENTRY__', N). */
inline ChatAnswerRml BuildMarkdownAnswer(std::string_view markdown) {
  MarkdownRml body = MarkdownToRml(markdown);
  return ChatAnswerRml{std::move(body.rml), std::move(body.links)};
}

/** The URL a click may open: in range and https. Everything else is dropped. */
inline std::optional<std::string> ResolveChatLink(const std::vector<std::string>& links, const int index) {
  if (index < 0 || static_cast<size_t>(index) >= links.size() || !IsHttpsUrl(links[static_cast<size_t>(index)])) {
    return std::nullopt;
  }
  return links[static_cast<size_t>(index)];
}

/** Links of a stored message after a restart, from its own Markdown text. */
inline std::vector<std::string> RecoverChatLinks(std::string_view stored_markdown) {
  return MarkdownToRml(stored_markdown).links;
}

} // namespace pbr
