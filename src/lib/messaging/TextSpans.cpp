#include "lib/messaging/TextSpans.h"

#include "common/Serialize.hpp"
#include "common/ValueJson.h"
#include "foundation/crypto/CryptoUtil.h"

#include <algorithm>
#include <set>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {
namespace text_spans {
namespace {

bool IsContinuationByte(const unsigned char c) { return (c & 0xC0) == 0x80; }

/** A byte offset that starts a UTF-8 character (or is the end of the text). */
bool OnCharBoundary(const std::string& text, const uint32_t offset) {
  return offset == text.size() || (offset < text.size() && !IsContinuationByte(static_cast<unsigned char>(text[offset])));
}

bool Marks(const TextSpan& span) { return span.Is(TextSpanKind::Mention) || span.Is(TextSpanKind::Everyone); }

} // namespace

Roe<void> CheckStructure(const std::vector<TextSpan>& spans) {
  if (spans.size() > kMaxSpans) {
    return Error("text_spans: too many spans");
  }
  std::set<std::string> mentioned;
  size_t everyone = 0;
  for (const TextSpan& span : spans) {
    if (span.Is(TextSpanKind::Mention)) {
      if (span.account_id.empty()) {
        return Error("text_spans: a mention names no account");
      }
      if (!mentioned.insert(span.account_id).second) {
        return Error("text_spans: an account mentioned twice");
      }
    } else if (span.Is(TextSpanKind::Everyone)) {
      ++everyone;
    }
  }
  if (mentioned.size() > kMaxMentions) {
    return Error("text_spans: too many mentions");
  }
  if (everyone > 1) {
    return Error("text_spans: more than one everyone span");
  }
  return {};
}

std::vector<TextSpan> KeepValidRanges(const std::string& text, std::vector<TextSpan> spans, size_t* dropped) {
  std::vector<TextSpan> in_text;
  for (TextSpan& span : spans) {
    const bool fits = span.start < span.end && span.end <= text.size() && OnCharBoundary(text, span.start) &&
                      OnCharBoundary(text, span.end);
    if (fits && (!Marks(span) || text[span.start] == '@')) {
      in_text.push_back(std::move(span));
    }
  }
  // Overlapping spans: neither is kept (which one the sender meant is not ours to guess).
  std::vector<bool> overlaps(in_text.size(), false);
  for (size_t i = 0; i < in_text.size(); ++i) {
    for (size_t j = i + 1; j < in_text.size(); ++j) {
      if (in_text[i].start < in_text[j].end && in_text[j].start < in_text[i].end) {
        overlaps[i] = overlaps[j] = true;
      }
    }
  }
  std::vector<TextSpan> kept;
  for (size_t i = 0; i < in_text.size(); ++i) {
    if (!overlaps[i]) {
      kept.push_back(std::move(in_text[i]));
    }
  }
  if (dropped) {
    *dropped = spans.size() - kept.size();
  }
  return kept;
}

std::vector<uint8_t> EncodeBody(const TextSpan& span) {
  if (span.Is(TextSpanKind::Mention)) {
    std::ostringstream oss;
    OutputArchive ar(oss);
    WireLenUtf8 id{span.account_id};
    ar & id;
    const std::string bytes = oss.str();
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
  }
  if (span.Is(TextSpanKind::Everyone)) {
    return {};
  }
  return span.body;
}

Roe<void> DecodeBody(TextSpan& span, const std::vector<uint8_t>& body) {
  if (span.Is(TextSpanKind::Mention)) {
    std::istringstream iss(std::string(body.begin(), body.end()));
    InputArchive ar(iss);
    WireLenUtf8 id;
    ar & id;
    if (ar.failed() || !ar.exactEnd()) {
      return Error("text_spans: malformed mention");
    }
    span.account_id = id.value;
    return {};
  }
  if (span.Is(TextSpanKind::Everyone)) {
    if (!body.empty()) {
      return Error("text_spans: an everyone span with a body");
    }
    return {};
  }
  span.body = body;
  return {};
}

std::string ToPayloadJson(const std::string& text, const std::vector<TextSpan>& spans) {
  Object payload;
  payload.set("text", text);
  if (!spans.empty()) {
    std::vector<Value> rows;
    for (const TextSpan& span : spans) {
      Object row;
      row.set("k", static_cast<int64_t>(span.kind));
      row.set("s", static_cast<int64_t>(span.start));
      row.set("e", static_cast<int64_t>(span.end));
      if (span.Is(TextSpanKind::Mention)) {
        row.set("id", span.account_id);
      } else if (!span.Is(TextSpanKind::Everyone)) {
        row.set("body", Base64Encode(span.body));
      }
      rows.push_back(ObjectValue(std::move(row)));
    }
    payload.set("spans", ArrayValue(std::move(rows)));
  }
  return DumpJson(payload);
}

std::vector<TextSpan> FromPayloadJson(const std::string& payload_json) {
  std::vector<TextSpan> spans;
  auto payload = TryParseObject(payload_json);
  const Array* rows = payload ? payload->getArray("spans") : nullptr;
  if (!rows) {
    return spans;
  }
  for (const Value& item : rows->elements) {
    const Object* row = asObject(item);
    if (!row) {
      continue;
    }
    TextSpan span;
    span.kind = static_cast<uint8_t>(row->getNonNegInt("k").value_or(0));
    span.start = static_cast<uint32_t>(row->getNonNegInt("s").value_or(0));
    span.end = static_cast<uint32_t>(row->getNonNegInt("e").value_or(0));
    span.account_id = row->getString("id").value_or("");
    if (auto body = row->getString("body")) {
      if (auto decoded = Base64Decode(*body)) {
        span.body = std::move(*decoded);
      }
    }
    if (span.kind != 0) {
      spans.push_back(std::move(span));
    }
  }
  return spans;
}

std::vector<std::string> MentionedAccounts(const std::vector<TextSpan>& spans) {
  std::vector<std::string> out;
  for (const TextSpan& span : spans) {
    if (span.Is(TextSpanKind::Mention)) {
      out.push_back(span.account_id);
    }
  }
  return out;
}

bool MentionsEveryone(const std::vector<TextSpan>& spans) {
  return std::any_of(spans.begin(), spans.end(), [](const TextSpan& s) { return s.Is(TextSpanKind::Everyone); });
}

} // namespace text_spans
} // namespace pbr
