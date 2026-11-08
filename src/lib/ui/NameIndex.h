#pragma once

#include <string>
#include <string_view>

namespace pbr {

/** The section of names that do not start with a letter A-Z or a Chinese character with a known reading. */
inline constexpr char kNameIndexOther = '#';

/**
 * The index letter of a name, as the phone contact lists section it: 'A'-'Z' for a name starting with a Latin letter
 * (case-blind) or with a CJK Unified Ideograph (U+4E00-U+9FFF, by the pinyin initial of its default reading);
 * '#' for everything else (digits, symbols, other scripts, an empty name). Leading spaces are skipped.
 */
char NameIndexLetter(std::string_view name);

/**
 * A key that sorts names within their index letter: ASCII letters lower-cased, and each Chinese character preceded
 * by its lower-case pinyin initial (王伟 before 王小明, as w < x), so names sort by their initials, then code point.
 * Compare keys with operator<; sort by NameIndexLetter first ('#' last) — NameIndexLess does both.
 */
std::string NameSortKey(std::string_view name);

/** Contact-list order: A-Z then '#', and by NameSortKey inside a letter. */
bool NameIndexLess(std::string_view a, std::string_view b);

/**
 * The pinyin initials of a name, lower-case: a Latin letter stands for itself, a Chinese character with a known
 * reading for its initial (张伟 -> "zw"); digits, symbols and spaces are skipped.
 */
std::string NameInitials(std::string_view name);

/**
 * Whether a contact field matches a search: the query (case-blind) is inside the text, or it is the start of the
 * text's initials ("zw" finds 张伟, "z" every name starting with Z). An empty query matches everything.
 */
bool TextMatchesQuery(std::string_view text, std::string_view query);

} // namespace pbr
