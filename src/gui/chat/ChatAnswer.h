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
constexpr size_t kMaxAnswerSources = 10;

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

/** Markdown body plus a compact Sources list; source URLs extend `links` (reusing an index when already linked). */
inline ChatAnswerRml BuildMarkdownAnswer(std::string_view markdown, const std::vector<BriefAiSource>& sources,
                                         const std::string& sources_label) {
  MarkdownRml body = MarkdownToRml(markdown);
  ChatAnswerRml out{std::move(body.rml), std::move(body.links)};

  std::string lines;
  size_t shown = 0;
  for (const BriefAiSource& source : sources) {
    if (shown >= kMaxAnswerSources) {
      break;
    }
    if (!IsHttpsUrl(source.url)) {
      continue;
    }
    auto it = std::find(out.links.begin(), out.links.end(), source.url);
    const size_t index = static_cast<size_t>(it - out.links.begin());
    if (it == out.links.end()) {
      out.links.push_back(source.url);
    }
    const std::string title = source.title.empty() ? UrlHost(source.url) : source.title;
    lines += "<p><span class=\"chat-link\" data-event-click=\"open_chat_link('__ENTRY__', " + std::to_string(index) +
             ")\">" + StructuredTextParser::EscapeText(title) + "</span></p>";
    ++shown;
  }
  if (shown > 0) {
    out.rml += "<div class=\"chat-sources\"><p class=\"muted\">" + StructuredTextParser::EscapeText(sources_label) +
               "</p>" + lines + "</div>";
  }
  return out;
}

/** The URL a click may open: in range and https. Everything else is dropped. */
inline std::optional<std::string> ResolveChatLink(const std::vector<std::string>& links, const int index) {
  if (index < 0 || static_cast<size_t>(index) >= links.size() || !IsHttpsUrl(links[static_cast<size_t>(index)])) {
    return std::nullopt;
  }
  return links[static_cast<size_t>(index)];
}

/** Links of a stored message after a restart: only its own Markdown text (sources are not persisted). */
inline std::vector<std::string> RecoverChatLinks(std::string_view stored_markdown) {
  return MarkdownToRml(stored_markdown).links;
}

} // namespace pbr
