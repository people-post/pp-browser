#include "domain/ai/MarkdownToRml.h"

#include "domain/ai/StructuredTextParser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pbr {
namespace {

constexpr size_t kMaxListDepth = 6;
constexpr size_t kMaxOpenInline = 32; // open emphasis/bracket markers at once (bounds output nesting)
constexpr size_t kMaxUrl = 2048;

using sv = std::string_view;

void AppendEscaped(std::string& out, sv text) { out += StructuredTextParser::EscapeText(std::string(text)); }

bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool IsAsciiPunct(char c) {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
// Non-ASCII bytes count as word characters so CJK / emoji neighbours behave like letters.
bool IsAsciiAlnum(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsWord(char c) { return IsAsciiAlnum(c) || static_cast<unsigned char>(c) >= 0x80; }
char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool MatchCi(sv s, size_t pos, sv lower_needle) {
  if (pos + lower_needle.size() > s.size()) {
    return false;
  }
  for (size_t k = 0; k < lower_needle.size(); ++k) {
    if (Lower(s[pos + k]) != lower_needle[k]) {
      return false;
    }
  }
  return true;
}

sv Trim(sv s) {
  while (!s.empty() && IsWs(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && IsWs(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

// ---------------------------------------------------------------------------------------------
// Inline parsing
// ---------------------------------------------------------------------------------------------

struct InlineContext {
  std::vector<std::string>& links;
  std::unordered_map<std::string, size_t> link_index;

  size_t Intern(const std::string& url) {
    auto it = link_index.find(url);
    if (it != link_index.end()) {
      return it->second;
    }
    links.push_back(url);
    link_index.emplace(url, links.size() - 1);
    return links.size() - 1;
  }
};

bool IsSafeHttpsUrl(sv url) {
  if (url.size() <= 8 || url.size() > kMaxUrl || !MatchCi(url, 0, "https://")) {
    return false;
  }
  for (char c : url) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      return false;
    }
  }
  return true;
}

struct Tok {
  enum Kind { Text, Delim, Bracket, Code, LinkOpen, LinkClose, Br };
  Kind kind = Text;
  std::string text; // Text/Code: content; Delim/Bracket: literal source; LinkClose: url for non-https
  int role = 0;     // Delim: 0 literal, 1 open, 2 close
  long link = -1;   // LinkOpen/LinkClose: index into links, or -1 for a non-https "link"
};

struct BracketEntry {
  size_t tok;
  bool image;
};

class InlineParser {
public:
  InlineParser(sv s, InlineContext& ctx) : s_(s), ctx_(ctx) {}

  std::string Render() {
    Tokenize();
    std::string out;
    for (const Tok& t : toks_) {
      switch (t.kind) {
      case Tok::Text:
        AppendEscaped(out, t.text);
        break;
      case Tok::Delim:
        if (t.role == 0) {
          AppendEscaped(out, t.text);
        } else {
          const bool strong = t.text.size() == 2;
          out += t.role == 1 ? (strong ? "<strong>" : "<em>") : (strong ? "</strong>" : "</em>");
        }
        break;
      case Tok::Bracket:
        AppendEscaped(out, t.text);
        break;
      case Tok::Code:
        out += "<span class=\"inline-code\">";
        AppendEscaped(out, t.text);
        out += "</span>";
        break;
      case Tok::LinkOpen:
        if (t.link >= 0) {
          out += "<span class=\"chat-link\" data-event-click=\"open_chat_link('__ENTRY__', " + std::to_string(t.link) +
                 ")\">";
        }
        break;
      case Tok::LinkClose:
        if (t.link >= 0) {
          out += "</span>";
        } else if (!t.text.empty()) {
          out += " (";
          AppendEscaped(out, t.text);
          out += ")";
        }
        break;
      case Tok::Br:
        out += "<br />";
        break;
      }
    }
    return out;
  }

private:
  sv s_;
  InlineContext& ctx_;
  std::vector<Tok> toks_;
  std::string buf_;
  std::array<std::vector<size_t>, 4> emph_; // open emphasis markers: *, **, _, __
  std::vector<BracketEntry> brackets_;
  size_t open_count_ = 0;
  size_t inactive_below_ = 0; // brackets below this stack index cannot become links (no links in links)
  std::unordered_map<size_t, size_t> code_fail_; // backtick run length -> no closer at or after this pos

  void Flush() {
    if (!buf_.empty()) {
      toks_.push_back({Tok::Text, std::move(buf_)});
      buf_.clear();
    }
  }

  void DemoteAbove(size_t tok) {
    for (auto& stack : emph_) {
      while (!stack.empty() && stack.back() > tok) {
        stack.pop_back();
        --open_count_;
      }
    }
  }

  size_t FindCodeClose(size_t from, size_t len) {
    auto fail = code_fail_.find(len);
    if (fail != code_fail_.end() && from >= fail->second) {
      return sv::npos;
    }
    size_t k = from;
    while (k < s_.size()) {
      if (s_[k] != '`') {
        ++k;
        continue;
      }
      size_t run = k;
      while (run < s_.size() && s_[run] == '`') {
        ++run;
      }
      if (run - k == len) {
        return k;
      }
      k = run;
    }
    auto [it, inserted] = code_fail_.emplace(len, from);
    if (!inserted) {
      it->second = std::min(it->second, from);
    }
    return sv::npos;
  }

  void Tokenize() {
    const size_t n = s_.size();
    size_t i = 0;
    while (i < n) {
      const char c = s_[i];
      if (c == '\\' && i + 1 < n && IsAsciiPunct(s_[i + 1])) {
        buf_ += s_[i + 1];
        i += 2;
      } else if (c == '\n') {
        Flush();
        toks_.push_back({Tok::Br, ""});
        ++i;
      } else if (c == '`') {
        i = HandleBackticks(i);
      } else if (c == '*' || c == '_') {
        i = HandleEmphasis(i);
      } else if (c == '[' || (c == '!' && i + 1 < n && s_[i + 1] == '[')) {
        const bool image = c == '!';
        const size_t width = image ? 2 : 1;
        if (open_count_ < kMaxOpenInline) {
          Flush();
          brackets_.push_back({toks_.size(), image});
          ++open_count_;
          toks_.push_back({Tok::Bracket, std::string(s_.substr(i, width))});
        } else {
          buf_.append(s_.substr(i, width));
        }
        i += width;
      } else if (c == ']') {
        i = HandleCloseBracket(i);
      } else if (c == 'h' && TryAutolink(i)) {
        i = autolink_end_;
      } else {
        buf_ += c;
        ++i;
      }
    }
    Flush();
  }

  size_t HandleBackticks(size_t i) {
    size_t j = i;
    while (j < s_.size() && s_[j] == '`') {
      ++j;
    }
    const size_t len = j - i;
    const size_t close = FindCodeClose(j, len);
    if (close == sv::npos) {
      buf_.append(len, '`');
      return j;
    }
    Flush();
    std::string content(s_.substr(j, close - j));
    std::replace(content.begin(), content.end(), '\n', ' ');
    if (content.size() >= 2 && content.front() == ' ' && content.back() == ' ' &&
        content.find_first_not_of(' ') != std::string::npos) {
      content = content.substr(1, content.size() - 2);
    }
    toks_.push_back({Tok::Code, std::move(content)});
    return close + len;
  }

  size_t HandleEmphasis(size_t i) {
    const char c = s_[i];
    size_t j = i;
    while (j < s_.size() && s_[j] == c) {
      ++j;
    }
    const bool star = c == '*';
    const char prev = i > 0 ? s_[i - 1] : ' ';
    const char next = j < s_.size() ? s_[j] : ' ';
    const bool can_open = !IsWs(next) && (star || !IsWord(prev));
    const bool can_close = !IsWs(prev) && (star || !IsWord(next));
    if (!can_open && !can_close) {
      buf_.append(j - i, c);
      return j;
    }
    Flush();
    // Marker order within a run: closing runs try `**` first so `***x***` nests as <em><strong>.
    const size_t pairs = (j - i) / 2;
    const bool odd = ((j - i) % 2) != 0;
    const bool closing_first = can_close && !can_open;
    std::vector<size_t> lens;
    lens.reserve(pairs + 1);
    if (odd && !closing_first) {
      lens.push_back(1);
    }
    lens.insert(lens.end(), pairs, 2);
    if (odd && closing_first) {
      lens.push_back(1);
    }
    for (size_t len : lens) {
      Tok t{Tok::Delim, std::string(len, c)};
      auto& stack = emph_[(star ? 0 : 2) + (len == 2 ? 1 : 0)];
      // A closer may not reach across an unclosed `[`: that would cross the link's span.
      const bool blocked = !stack.empty() && !brackets_.empty() && stack.back() < brackets_.back().tok;
      if (can_close && !stack.empty() && !blocked) {
        const size_t open = stack.back();
        stack.pop_back();
        --open_count_;
        toks_[open].role = 1;
        t.role = 2;
        DemoteAbove(open);
      } else if (can_open && open_count_ < kMaxOpenInline) {
        stack.push_back(toks_.size());
        ++open_count_;
      }
      toks_.push_back(std::move(t));
    }
    return j;
  }

  // Parses `(dest)` starting at s_[open] == '('; returns index of the closing ')' or npos.
  size_t FindDestEnd(size_t open) const {
    int depth = 1;
    const size_t limit = std::min(s_.size(), open + kMaxUrl + 2);
    for (size_t k = open + 1; k < limit; ++k) {
      const char c = s_[k];
      if (c == '\\' && k + 1 < s_.size() && IsAsciiPunct(s_[k + 1])) {
        ++k;
      } else if (IsWs(c)) {
        return sv::npos;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')' && --depth == 0) {
        return k;
      }
    }
    return sv::npos;
  }

  size_t HandleCloseBracket(size_t i) {
    Flush();
    if (!brackets_.empty()) {
      const BracketEntry b = brackets_.back();
      const bool active = b.image || brackets_.size() - 1 >= inactive_below_;
      brackets_.pop_back();
      --open_count_;
      inactive_below_ = std::min(inactive_below_, brackets_.size());
      if (active && i + 1 < s_.size() && s_[i + 1] == '(') {
        const size_t end = FindDestEnd(i + 1);
        if (end != sv::npos) {
          FinishLink(b, s_.substr(i + 2, end - i - 2));
          return end + 1;
        }
      }
    }
    buf_ += ']';
    return i + 1;
  }

  void FinishLink(const BracketEntry& b, sv raw_dest) {
    DemoteAbove(b.tok);
    if (b.image) {
      // Only the alt text survives; nothing that could load a remote resource is emitted.
      for (size_t k = b.tok; k < toks_.size(); ++k) {
        Tok& t = toks_[k];
        switch (t.kind) {
        case Tok::Text:
          break;
        case Tok::Code:
          t.kind = Tok::Text;
          break;
        case Tok::Br:
          t.kind = Tok::Text;
          t.text = " ";
          break;
        case Tok::Delim:
          if (t.role != 0) {
            t.text.clear(); // resolved emphasis markers inside alt text vanish
          }
          t.kind = Tok::Text;
          t.role = 0;
          break;
        case Tok::Bracket:
          if (k == b.tok) {
            t.text.clear();
          }
          t.kind = Tok::Text;
          break;
        case Tok::LinkOpen:
        case Tok::LinkClose:
          t.text.clear();
          t.kind = Tok::Text;
          break;
        }
      }
      return;
    }
    std::string url;
    for (size_t k = 0; k < raw_dest.size(); ++k) {
      if (raw_dest[k] == '\\' && k + 1 < raw_dest.size() && IsAsciiPunct(raw_dest[k + 1])) {
        ++k;
      }
      url += raw_dest[k];
    }
    const bool https = IsSafeHttpsUrl(url);
    const long index = https ? static_cast<long>(ctx_.Intern(url)) : -1;
    toks_[b.tok].kind = Tok::LinkOpen;
    toks_[b.tok].link = index;
    Tok close{Tok::LinkClose, https ? "" : std::move(url)};
    close.link = index;
    toks_.push_back(std::move(close));
    inactive_below_ = brackets_.size();
  }

  size_t autolink_end_ = 0;

  // Not inside an open `[`: it may still become a link, and links cannot nest.
  bool TryAutolink(size_t i) {
    if (!brackets_.empty() || !MatchCi(s_, i, "https://") || (i > 0 && IsAsciiAlnum(s_[i - 1]))) {
      return false;
    }
    if (i >= 2 && s_[i - 1] == '(' && s_[i - 2] == ']') {
      return false; // unfinished `[label](https://...`: keep it literal
    }
    const size_t limit = std::min(s_.size(), i + kMaxUrl + 1);
    size_t j = i + 8;
    // The URL ends at the first non-ASCII byte: Chinese text runs on without spaces.
    while (j < limit && !IsWs(s_[j]) && s_[j] != '<' && static_cast<unsigned char>(s_[j]) < 0x80) {
      ++j;
    }
    if (j - i > kMaxUrl) {
      return false;
    }
    long open_parens = 0;
    long close_parens = 0;
    for (size_t k = i; k < j; ++k) {
      open_parens += s_[k] == '(';
      close_parens += s_[k] == ')';
    }
    while (j > i + 8) {
      const char last = s_[j - 1];
      if (last == '.' || last == ',' || last == ';' || last == ':' || last == '!' || last == '?' || last == '\'' ||
          last == '"' || last == '*' || last == '_' || last == '~') {
        --j;
      } else if (last == ')' && close_parens > open_parens) {
        --close_parens;
        --j;
      } else {
        break;
      }
    }
    std::string url(s_.substr(i, j - i));
    if (!IsSafeHttpsUrl(url)) {
      return false;
    }
    Flush();
    Tok open{Tok::LinkOpen, ""};
    open.link = static_cast<long>(ctx_.Intern(url));
    Tok close{Tok::LinkClose, ""};
    close.link = open.link;
    toks_.push_back(std::move(open));
    toks_.push_back({Tok::Text, std::move(url)});
    toks_.push_back(std::move(close));
    autolink_end_ = j;
    return true;
  }
};

std::string Inline(sv text, InlineContext& ctx) { return InlineParser(text, ctx).Render(); }

// ---------------------------------------------------------------------------------------------
// Block parsing
// ---------------------------------------------------------------------------------------------

using Lines = std::vector<sv>;

bool IsBlank(sv l) { return Trim(l).empty(); }

size_t Indent(sv l) {
  size_t ind = 0;
  for (char c : l) {
    if (c == ' ') {
      ++ind;
    } else if (c == '\t') {
      ind += 4;
    } else {
      break;
    }
  }
  return ind;
}

bool FenceOpen(sv l, char& ch) {
  l.remove_prefix(std::min(l.find_first_not_of(" \t"), l.size()));
  if (l.size() < 3 || (l[0] != '`' && l[0] != '~')) {
    return false;
  }
  ch = l[0];
  size_t run = 0;
  while (run < l.size() && l[run] == ch) {
    ++run;
  }
  if (run < 3) {
    return false;
  }
  return ch != '`' || l.find('`', run) == sv::npos; // ```code``` on one line is inline code
}

bool FenceClose(sv l, char ch) {
  l = Trim(l);
  return l.size() >= 3 && l.find_first_not_of(ch) == sv::npos;
}

bool Heading(sv l, int& level, sv& content) {
  if (Indent(l) > 3) {
    return false;
  }
  l.remove_prefix(std::min(l.find_first_not_of(" \t"), l.size()));
  size_t run = 0;
  while (run < l.size() && l[run] == '#') {
    ++run;
  }
  if (run == 0 || run > 6 || (run < l.size() && l[run] != ' ' && l[run] != '\t')) {
    return false;
  }
  level = static_cast<int>(std::min<size_t>(run, 3));
  content = Trim(l.substr(run));
  return true;
}

bool IsThematicBreak(sv l) {
  char mark = 0;
  size_t count = 0;
  for (char c : l) {
    if (c == ' ' || c == '\t') {
      continue;
    }
    if ((c != '-' && c != '*' && c != '_') || (mark != 0 && c != mark)) {
      return false;
    }
    mark = c;
    ++count;
  }
  return count >= 3;
}

bool Quote(sv l, sv& content) {
  if (Indent(l) > 3) {
    return false;
  }
  l.remove_prefix(std::min(l.find_first_not_of(" \t"), l.size()));
  if (l.empty() || l[0] != '>') {
    return false;
  }
  l.remove_prefix(1);
  content = Trim(l);
  return true;
}

struct Item {
  size_t indent = 0;
  bool ordered = false;
  sv content;
};

bool ParseItem(sv l, Item& item) {
  item.indent = Indent(l);
  size_t p = std::min(l.find_first_not_of(" \t"), l.size());
  if (p >= l.size()) {
    return false;
  }
  size_t q = 0;
  if (l[p] == '-' || l[p] == '*' || l[p] == '+') {
    item.ordered = false;
    q = p + 1;
  } else if (IsDigit(l[p])) {
    size_t r = p;
    while (r < l.size() && IsDigit(l[r]) && r - p < 9) {
      ++r;
    }
    if (r >= l.size() || l[r] != '.') {
      return false;
    }
    item.ordered = true;
    q = r + 1;
  } else {
    return false;
  }
  if (q >= l.size() || (l[q] != ' ' && l[q] != '\t')) {
    return false;
  }
  item.content = Trim(l.substr(q));
  return true;
}

sv StripEdgePipes(sv l) {
  l = Trim(l);
  if (!l.empty() && l.front() == '|') {
    l.remove_prefix(1);
  }
  if (!l.empty() && l.back() == '|' && (l.size() < 2 || l[l.size() - 2] != '\\')) {
    l.remove_suffix(1);
  }
  return l;
}

std::vector<sv> SplitRow(sv l) {
  l = StripEdgePipes(l);
  std::vector<sv> cells;
  size_t start = 0;
  for (size_t k = 0; k < l.size(); ++k) {
    if (l[k] == '\\') {
      ++k;
    } else if (l[k] == '|') {
      cells.push_back(Trim(l.substr(start, k - start)));
      start = k + 1;
    }
  }
  cells.push_back(Trim(l.substr(start)));
  return cells;
}

bool IsDelimCell(sv c) {
  if (!c.empty() && c.front() == ':') {
    c.remove_prefix(1);
  }
  if (!c.empty() && c.back() == ':') {
    c.remove_suffix(1);
  }
  return !c.empty() && c.find_first_not_of('-') == sv::npos;
}

bool TableStart(const Lines& lines, size_t i) {
  if (i + 1 >= lines.size() || lines[i].find('|') == sv::npos || lines[i + 1].find('|') == sv::npos) {
    return false;
  }
  const std::vector<sv> delim = SplitRow(lines[i + 1]);
  if (!std::all_of(delim.begin(), delim.end(), IsDelimCell)) {
    return false;
  }
  return SplitRow(lines[i]).size() == delim.size();
}

bool StartsBlock(const Lines& lines, size_t i) {
  const sv l = lines[i];
  char ch = 0;
  int level = 0;
  sv content;
  Item item;
  return IsBlank(l) || FenceOpen(l, ch) || Heading(l, level, content) || IsThematicBreak(l) || Quote(l, content) ||
         ParseItem(l, item) || TableStart(lines, i);
}

void EmitList(const Lines& lines, size_t& i, InlineContext& ctx, std::string& out) {
  struct Level {
    size_t indent;
    bool ordered;
  };
  std::vector<Level> stack;
  const auto open_list = [&](bool ordered) { out += ordered ? "<ol>" : "<ul>"; };
  const auto close_list = [&](const Level& lv) { out += lv.ordered ? "</li></ol>" : "</li></ul>"; };

  while (i < lines.size()) {
    Item item;
    if (!ParseItem(lines[i], item)) {
      if (!IsBlank(lines[i])) {
        break;
      }
      size_t k = i;
      while (k < lines.size() && IsBlank(lines[k])) {
        ++k;
      }
      if (k < lines.size() && ParseItem(lines[k], item)) {
        i = k; // a blank line between items does not end the list
      } else {
        break;
      }
    }
    if (stack.empty()) {
      open_list(item.ordered);
      stack.push_back({item.indent, item.ordered});
    } else if (item.indent >= stack.back().indent + 2 && stack.size() < kMaxListDepth) {
      open_list(item.ordered);
      stack.push_back({item.indent, item.ordered});
    } else {
      while (stack.size() > 1 && item.indent < stack.back().indent) {
        close_list(stack.back());
        stack.pop_back();
      }
      if (stack.back().ordered != item.ordered) {
        close_list(stack.back());
        open_list(item.ordered);
        stack.back().ordered = item.ordered;
      } else {
        out += "</li>";
      }
    }
    out += "<li>";

    std::string text(item.content);
    ++i;
    char ch = 0;
    Item next;
    while (i < lines.size() && !IsBlank(lines[i]) && Indent(lines[i]) >= item.indent + 2 &&
           !ParseItem(lines[i], next) && !FenceOpen(lines[i], ch)) {
      text += '\n';
      text += Trim(lines[i]);
      ++i;
    }
    out += Inline(text, ctx);
  }
  for (size_t k = stack.size(); k > 0; --k) {
    close_list(stack[k - 1]);
  }
}

void EmitTable(const Lines& lines, size_t& i, InlineContext& ctx, std::string& out) {
  const std::vector<sv> header = SplitRow(lines[i]);
  out += "<table class=\"chat-table\"><thead><tr>";
  for (sv cell : header) {
    out += "<th>" + Inline(cell, ctx) + "</th>";
  }
  out += "</tr></thead><tbody>";
  i += 2;
  for (; i < lines.size() && !IsBlank(lines[i]) && lines[i].find('|') != sv::npos; ++i) {
    std::vector<sv> cells = SplitRow(lines[i]);
    cells.resize(header.size());
    out += "<tr>";
    for (sv cell : cells) {
      out += "<td>" + Inline(cell, ctx) + "</td>";
    }
    out += "</tr>";
  }
  out += "</tbody></table>";
}

Lines SplitLines(sv text) {
  Lines lines;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == sv::npos) {
      end = text.size();
    }
    sv line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.push_back(line);
    start = end + 1;
  }
  return lines;
}

// The validator rejects these substrings anywhere, even as harmless text; a numeric entity for
// one character renders identically and breaks the match.
std::string DefuseForbiddenText(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t k = 0; k < in.size();) {
    if (Lower(in[k]) == 'j' && MatchCi(in, k, "javascript:")) {
      out.append(in, k, 10);
      out += "&#58;";
      k += 11;
    } else if (Lower(in[k]) == 'o' && MatchCi(in, k, "onclick=")) {
      out.append(in, k, 7);
      out += "&#61;";
      k += 8;
    } else {
      out += in[k++];
    }
  }
  return out;
}

} // namespace

MarkdownRml MarkdownToRml(std::string_view markdown) {
  MarkdownRml result;
  InlineContext ctx{result.links, {}};
  const Lines lines = SplitLines(markdown);
  std::string out;

  size_t i = 0;
  while (i < lines.size()) {
    const sv l = lines[i];
    char fence = 0;
    int level = 0;
    sv content;
    Item item;
    if (IsBlank(l)) {
      ++i;
    } else if (FenceOpen(l, fence)) {
      out += "<div class=\"code-block\">";
      bool first = true;
      for (++i; i < lines.size() && !FenceClose(lines[i], fence); ++i) {
        if (!first) {
          out += '\n';
        }
        AppendEscaped(out, lines[i]);
        first = false;
      }
      if (i < lines.size()) {
        ++i; // closing fence
      }
      out += "</div>";
    } else if (Heading(l, level, content)) {
      const std::string tag = "h" + std::to_string(level);
      out += "<" + tag + ">" + Inline(content, ctx) + "</" + tag + ">";
      ++i;
    } else if (IsThematicBreak(l)) {
      ++i;
    } else if (Quote(l, content)) {
      std::string text;
      for (; i < lines.size() && Quote(lines[i], content); ++i) {
        if (!content.empty()) {
          text += text.empty() ? "" : "\n";
          text += content;
        }
      }
      out += "<blockquote class=\"chat-quote\"><p>" + Inline(text, ctx) + "</p></blockquote>";
    } else if (ParseItem(l, item)) {
      EmitList(lines, i, ctx, out);
    } else if (TableStart(lines, i)) {
      EmitTable(lines, i, ctx, out);
    } else {
      std::string text(Trim(l));
      for (++i; i < lines.size() && !StartsBlock(lines, i); ++i) {
        text += '\n';
        text += Trim(lines[i]);
      }
      out += "<p>" + Inline(text, ctx) + "</p>";
    }
  }
  result.rml = DefuseForbiddenText(out);
  return result;
}

} // namespace pbr
