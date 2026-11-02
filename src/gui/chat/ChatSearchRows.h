#pragma once

#include "lib/ui/ChatWidgetTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pbr {

namespace search_rows_detail {

inline char FoldAscii(const char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/** Index just past the tag starting at `start` ('<'), quoted attribute values skipped; the end when unclosed. */
inline size_t TagEnd(const std::string& rml, const size_t start) {
  char quote = 0;
  for (size_t i = start + 1; i < rml.size(); ++i) {
    const char c = rml[i];
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i + 1;
    }
  }
  return rml.size();
}

/** Index just past the entity starting at `start` ('&'), or `start` when it is not one ("&amp;", "&#39;"). */
inline size_t EntityEnd(const std::string& rml, const size_t start) {
  constexpr size_t kMaxEntity = 32;
  for (size_t i = start + 1; i < rml.size() && i - start <= kMaxEntity; ++i) {
    const char c = rml[i];
    if (c == ';') {
      return i > start + 1 ? i + 1 : start;
    }
    const bool name = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#';
    if (!name) {
      return start;
    }
  }
  return start;
}

/** `run` (plain text, no tag or entity) with every occurrence of `folded_query` wrapped in `open` … `</span>`. */
inline void AppendMarkedRun(std::string& out, const std::string_view run, const std::string& folded_query,
                            const std::string& open) {
  std::string folded(run);
  for (char& c : folded) {
    c = FoldAscii(c);
  }
  size_t from = 0;
  for (size_t at = folded.find(folded_query); at != std::string::npos; at = folded.find(folded_query, from)) {
    out.append(run.substr(from, at - from));
    out += open;
    out.append(run.substr(at, folded_query.size()));
    out += "</span>";
    from = at + folded_query.size();
  }
  out.append(run.substr(from));
}

} // namespace search_rows_detail

/**
 * `content_rml` with each occurrence of `folded_query` (FoldForSearch: ASCII lowered, trimmed) in its text wrapped in
 * `<span class="search-mark">` (`current`: also `search-mark-current`). Only text between tags is searched, never a
 * tag, an attribute or an entity; a match across a tag or an entity is not marked. Bytes are compared as they are
 * (ASCII letters folded): UTF-8 never matches a whole character sequence off its boundaries. An empty query, or one
 * holding '<', '>' or '&', leaves the markup unchanged.
 */
inline std::string MarkSearchHits(const std::string& content_rml, const std::string& folded_query, const bool current) {
  using namespace search_rows_detail;
  if (folded_query.empty() || folded_query.find_first_of("<>&") != std::string::npos) {
    return content_rml;
  }
  const std::string open =
      current ? "<span class=\"search-mark search-mark-current\">" : "<span class=\"search-mark\">";
  std::string out;
  out.reserve(content_rml.size() + 2 * (open.size() + 7));
  size_t i = 0;
  while (i < content_rml.size()) {
    const char c = content_rml[i];
    if (c == '<') {
      const size_t end = TagEnd(content_rml, i);
      out.append(content_rml, i, end - i);
      i = end;
      continue;
    }
    if (c == '&') {
      const size_t end = EntityEnd(content_rml, i);
      const size_t stop = end > i ? end : i + 1; // a lone '&' is text, but never part of a match
      out.append(content_rml, i, stop - i);
      i = stop;
      continue;
    }
    size_t run_end = content_rml.find_first_of("<&", i);
    if (run_end == std::string::npos) {
      run_end = content_rml.size();
    }
    AppendMarkedRun(out, std::string_view(content_rml).substr(i, run_end - i), folded_query, open);
    i = run_end;
  }
  return out;
}

/** The row as built: its search marks taken off. */
inline void ClearSearchMarks(MessageDisplayRow& row) {
  if (row.search_source_rml) {
    row.content_rml = std::move(*row.search_source_rml);
    row.search_source_rml.reset();
  }
  row.search_hit = false;
  row.search_current = false;
}

/**
 * Search inside the chat: the rows of `hit_ids` get their matching text marked (`folded_query`), the one of
 * `current_id` in the deeper colour. Marks from an earlier query or step are taken off first.
 */
inline void MarkSearchRows(std::vector<MessageDisplayRow>& rows, const std::unordered_set<std::string>& hit_ids,
                           const std::string& current_id, const std::string& folded_query) {
  for (MessageDisplayRow& row : rows) {
    ClearSearchMarks(row);
    const std::string id = row.message_id.c_str();
    row.search_hit = hit_ids.count(id) > 0;
    row.search_current = row.search_hit && id == current_id;
    if (!row.search_hit) {
      continue;
    }
    std::string marked = MarkSearchHits(row.content_rml.c_str(), folded_query, row.search_current);
    if (marked != row.content_rml.c_str()) {
      row.search_source_rml = std::string(row.content_rml.c_str());
      row.content_rml = marked.c_str();
    }
  }
}

} // namespace pbr
