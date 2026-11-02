#include "feature/presenters/ThreadSearch.h"

#include "lib/ai/StructuredTextParser.h"
#include "lib/messaging/QuoteReply.h"

#include <algorithm>

namespace pbr {

std::string PlainTextOf(const ThreadMessage& message) {
  // Local-pipeline AI answers are stored as UI block documents.
  if (const auto prose = StructuredTextParser::PlainTextIfBlocks(message.text)) {
    return *prose;
  }
  // A reply that quotes another message: only what this message itself says.
  if (const auto parts = SplitQuoteReply(message.text)) {
    return parts->reply;
  }
  return message.text;
}

std::string FoldForSearch(const std::string_view text) {
  const auto is_space = [](const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && is_space(text[begin])) {
    ++begin;
  }
  while (end > begin && is_space(text[end - 1])) {
    --end;
  }
  std::string out(text.substr(begin, end - begin));
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::vector<ThreadSearchEntry> BuildSearchIndex(const std::vector<ThreadMessage>& messages) {
  std::vector<ThreadSearchEntry> index;
  index.reserve(messages.size());
  for (const ThreadMessage& message : messages) {
    if (message.content_type != ChatContentType::Text) {
      continue;
    }
    std::string folded = FoldForSearch(PlainTextOf(message));
    if (folded.empty()) {
      continue;
    }
    index.push_back({message.id, message.display_order, std::move(folded)});
  }
  return index;
}

std::vector<ThreadSearchHit> FindHits(const std::vector<ThreadSearchEntry>& index, const std::string_view query) {
  const std::string needle = FoldForSearch(query);
  std::vector<ThreadSearchHit> hits;
  if (needle.empty()) {
    return hits;
  }
  for (const ThreadSearchEntry& entry : index) {
    if (entry.folded_text.find(needle) != std::string::npos) {
      hits.push_back({entry.message_id, entry.display_order});
    }
  }
  std::stable_sort(hits.begin(), hits.end(), [](const ThreadSearchHit& a, const ThreadSearchHit& b) {
    return a.display_order > b.display_order;
  });
  return hits;
}

size_t RealignCurrent(const std::vector<ThreadSearchHit>& old_hits, const size_t old_current,
                      const std::vector<ThreadSearchHit>& new_hits) {
  if (old_current >= old_hits.size()) {
    return 0;
  }
  const std::string& id = old_hits[old_current].message_id;
  for (size_t i = 0; i < new_hits.size(); ++i) {
    if (new_hits[i].message_id == id) {
      return i;
    }
  }
  return 0;
}

} // namespace pbr
