#include "lib/ui/NameIndex.h"

#include <cstddef>
#include <cstdint>

namespace pbr {

namespace {

constexpr char32_t kCjkFirst = 0x4E00;
constexpr char32_t kCjkLast = 0x9FFF;

// One letter per code point from kCjkFirst to kCjkLast ('#' = no reading); see scripts/ui/gen_pinyin_initials.swift.
constexpr char kPinyinInitials[] =
#include "lib/ui/PinyinInitials.inc"
    ;
static_assert(sizeof(kPinyinInitials) - 1 == kCjkLast - kCjkFirst + 1, "one initial per CJK code point");

/** Decodes the code point at `pos` and moves past it; a malformed byte decodes as itself (never 0 progress). */
char32_t NextCodePoint(const std::string_view text, size_t& pos) {
  const auto byte = [&](const size_t i) { return static_cast<unsigned char>(text[i]); };
  const unsigned char lead = byte(pos);
  size_t length = 1;
  char32_t code = lead;
  if (lead >= 0xF0 && lead < 0xF8) {
    length = 4;
    code = lead & 0x07;
  } else if (lead >= 0xE0) {
    length = 3;
    code = lead & 0x0F;
  } else if (lead >= 0xC0) {
    length = 2;
    code = lead & 0x1F;
  }
  if (length > 1) {
    if (pos + length > text.size()) {
      ++pos;
      return lead;
    }
    for (size_t i = 1; i < length; ++i) {
      if ((byte(pos + i) & 0xC0) != 0x80) {
        ++pos;
        return lead;
      }
      code = (code << 6) | (byte(pos + i) & 0x3F);
    }
  }
  pos += length;
  return code;
}

/** 'A'-'Z' for a Latin letter or a Chinese character with a known reading, else '#'. */
char LetterOf(const char32_t code) {
  if (code >= 'a' && code <= 'z') {
    return static_cast<char>(code - 'a' + 'A');
  }
  if (code >= 'A' && code <= 'Z') {
    return static_cast<char>(code);
  }
  if (code >= kCjkFirst && code <= kCjkLast) {
    return kPinyinInitials[code - kCjkFirst];
  }
  return kNameIndexOther;
}

} // namespace

char NameIndexLetter(const std::string_view name) {
  size_t pos = 0;
  while (pos < name.size() && name[pos] == ' ') {
    ++pos;
  }
  if (pos >= name.size()) {
    return kNameIndexOther;
  }
  return LetterOf(NextCodePoint(name, pos));
}

std::string NameSortKey(const std::string_view name) {
  std::string key;
  key.reserve(name.size() * 2);
  size_t pos = 0;
  while (pos < name.size()) {
    const size_t start = pos;
    const char32_t code = NextCodePoint(name, pos);
    if (code >= 'A' && code <= 'Z') {
      key.push_back(static_cast<char>(code - 'A' + 'a'));
      continue;
    }
    if (code >= kCjkFirst && code <= kCjkLast) {
      const char letter = LetterOf(code);
      if (letter != kNameIndexOther) {
        key.push_back(static_cast<char>(letter - 'A' + 'a'));
      }
    }
    key.append(name.substr(start, pos - start));
  }
  return key;
}

std::string NameInitials(const std::string_view name) {
  std::string initials;
  size_t pos = 0;
  while (pos < name.size()) {
    const char letter = LetterOf(NextCodePoint(name, pos));
    if (letter != kNameIndexOther) {
      initials.push_back(static_cast<char>(letter - 'A' + 'a'));
    }
  }
  return initials;
}

namespace {

std::string LowerAscii(const std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

} // namespace

bool TextMatchesQuery(const std::string_view text, const std::string_view query) {
  if (query.empty()) {
    return true;
  }
  const std::string needle = LowerAscii(query);
  if (LowerAscii(text).find(needle) != std::string::npos) {
    return true;
  }
  return NameInitials(text).rfind(needle, 0) == 0;
}

bool NameIndexLess(const std::string_view a, const std::string_view b) {
  const char la = NameIndexLetter(a);
  const char lb = NameIndexLetter(b);
  if (la != lb) {
    if (la == kNameIndexOther || lb == kNameIndexOther) {
      return lb == kNameIndexOther;
    }
    return la < lb;
  }
  return NameSortKey(a) < NameSortKey(b);
}

} // namespace pbr
