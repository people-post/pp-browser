#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a span of a message's text marks (WIRE_SCHEMAS § text_spans, group-chat G015). */
enum class TextSpanKind : uint8_t {
  /** A member, by account id. */
  Mention = 1,
  /** Everyone in the group (the owner's to use). */
  Everyone = 2,
};

/** A marked range of a message's text: `[start, end)` in UTF-8 bytes. */
struct TextSpan {
  /** A TextSpanKind, or a kind this build does not know (kept as received, not acted on). */
  uint8_t kind = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  /** Mention: the member's account id. */
  std::string account_id;
  /** A kind this build does not know: its body as received, written back unchanged. */
  std::vector<uint8_t> body;

  bool Is(TextSpanKind k) const { return kind == static_cast<uint8_t>(k); }
  bool operator==(const TextSpan&) const = default;
};

namespace text_spans {

inline constexpr size_t kMaxSpans = 64;
/** The group size limit (G014): no message mentions more members than a group has. */
inline constexpr size_t kMaxMentions = 50;

/**
 * Structure (WIRE_SCHEMAS level 1): at most kMaxSpans spans and kMaxMentions mentions, one mention per
 * account, at most one everyone span, a mention names an account. An error rejects the whole message.
 */
Roe<void> CheckStructure(const std::vector<TextSpan>& spans);

/**
 * Ranges (level 2): the spans whose range lies in `text`, is not empty, starts and ends on a character
 * boundary, overlaps no other span, and — for a mention or everyone — starts with `@`. The others are
 * dropped (`dropped` counts them); the text stays as it is.
 */
std::vector<TextSpan> KeepValidRanges(const std::string& text, std::vector<TextSpan> spans, size_t* dropped = nullptr);

/** A known kind's body on the wire (mention: LenUtf8 account id; everyone: empty); unknown: as received. */
std::vector<uint8_t> EncodeBody(const TextSpan& span);
/** A span's kind-specific fields from its wire body; an error for a known kind whose body is malformed. */
Roe<void> DecodeBody(TextSpan& span, const std::vector<uint8_t>& body);

/** The local form: `{ "text", "spans": [{ "k", "s", "e", "id"?, "body"? }] }` (no `spans` when none). */
std::string ToPayloadJson(const std::string& text, const std::vector<TextSpan>& spans);
/** The spans a stored text message carries (none for plain text or unreadable JSON). */
std::vector<TextSpan> FromPayloadJson(const std::string& payload_json);

/** The account ids a message mentions (in span order). */
std::vector<std::string> MentionedAccounts(const std::vector<TextSpan>& spans);
bool MentionsEveryone(const std::vector<TextSpan>& spans);

} // namespace text_spans

} // namespace pbr
