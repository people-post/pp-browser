#include "domain/ai/AppActionClassifier.h"

#include "domain/ai/AppActionWords.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>

namespace pbr {
namespace {

constexpr size_t kZhMaxChars = 80;      // code points, after whitespace is stripped
constexpr size_t kZhMaxDistance = 12;   // verb/object distance, in code points
constexpr size_t kZhPrepMaxTail = 2;    // after a preposition-style starter (给/和/跟) at most "吧" / "一下" may follow
constexpr size_t kZhBareMaxChars = 10;  // a bare settings-read phrase
constexpr size_t kEnMaxChars = 160;
constexpr size_t kEnMaxDistance = 5;    // words from the verb to the object
constexpr size_t kEnBareMaxWords = 5;   // a bare settings-read phrase

using Text = std::u32string;
using View = std::u32string_view;

Text Decode(std::string_view in) {
  Text out;
  for (size_t i = 0; i < in.size();) {
    const auto b = static_cast<unsigned char>(in[i]);
    const size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 0;
    char32_t cp = 0xFFFD;
    if (n > 0 && i + n <= in.size()) {
      cp = n == 1 ? b : b & (0xFFu >> (n + 1));
      for (size_t k = 1; k < n; ++k) {
        const auto c = static_cast<unsigned char>(in[i + k]);
        if ((c & 0xC0) != 0x80) {
          cp = 0xFFFD;
          break;
        }
        cp = (cp << 6) | (c & 0x3F);
      }
    }
    out.push_back(cp);
    i += n > 0 ? n : 1;
  }
  return out;
}

bool IsCjk(const char32_t c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
         (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x2FFFF);
}

// Python's str.isspace set.
bool IsSpace(const char32_t c) {
  return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

char32_t Lower(const char32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

bool StartsWith(const View s, const View p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }

bool Contains(const View s, const View p) { return s.find(p) != View::npos; }

// ---------------------------------------------------------------------------------------------------------
// Chinese: port of brief_AI backend/tools/app_actions.py. Offsets and lengths are in code points.
// ---------------------------------------------------------------------------------------------------------

struct ZhEntry {
  std::string_view tool;
  bool settings_read = false;
  std::vector<Text> verbs, objects, phrases;
};

const std::vector<ZhEntry>& ZhTable() {
  static const std::vector<ZhEntry> table = [] {
    const auto norm = [](std::string_view s) {
      Text out;
      for (const char32_t c : Decode(s)) {
        if (!IsSpace(c)) {
          out.push_back(Lower(c));
        }
      }
      return out;
    };
    const auto all = [&](const std::vector<std::string_view>& words) {
      std::vector<Text> out;
      for (const auto w : words) {
        out.push_back(norm(w));
      }
      return out;
    };
    std::vector<ZhEntry> t;
    for (const AppActionWords& w : AppActionTable()) {
      t.push_back(ZhEntry{w.tool, w.settings_read, all(w.zh_verbs), all(w.zh_objects), all(w.zh_phrases)});
    }
    return t;
  }();
  return table;
}

// First matching prefix, in the given order (regex alternation semantics); returns its length or 0.
size_t MatchAny(const View s, const std::initializer_list<View> prefixes) {
  for (const View p : prefixes) {
    if (StartsWith(s, p)) {
      return p.size();
    }
  }
  return 0;
}

size_t MatchStarter(const View s) {
  return MatchAny(s, {U"请", U"帮我", U"帮忙", U"给我", U"给", U"替我", U"麻烦", U"把", U"让", U"和", U"跟", U"能不能",
                      U"能否", U"可不可以", U"我要", U"我想要", U"我想"});
}

size_t MatchPronoun(const View s) {
  return MatchAny(s, {U"他", U"她", U"它", U"这个人", U"这个", U"那个", U"这些", U"这位", U"此人"});
}

bool MatchOpinion(const View s) {
  return MatchAny(s, {U"我觉得", U"我认为", U"我看", U"我听说", U"我想知道", U"我想了解", U"我想问", U"我想请教", U"我好奇",
                      U"我感觉"}) > 0;
}

// ^(在|用)?((pp|app)(?![a-z])|这里|这个软件|这个应用|界面|设置里|通讯录)
bool MatchAppRef(const View s) {
  const auto body = [](const View r) {
    for (const View p : {View(U"pp"), View(U"app")}) {
      if (StartsWith(r, p) && !(r.size() > p.size() && r[p.size()] >= 'a' && r[p.size()] <= 'z')) {
        return true;
      }
    }
    return MatchAny(r, {U"这里", U"这个软件", U"这个应用", U"界面", U"设置里", U"通讯录"}) > 0;
  };
  if (!s.empty() && (s[0] == U'在' || s[0] == U'用') && body(s.substr(1))) {
    return true;
  }
  return body(s);
}

// ^(怎么|如何|怎样|咋)(?!评价|看待|看|理解|解读)
bool MatchHowTo(const View s) {
  const size_t n = MatchAny(s, {U"怎么", U"如何", U"怎样", U"咋"});
  return n > 0 && MatchAny(s.substr(n), {U"评价", U"看待", U"看", U"理解", U"解读"}) == 0;
}

// 我(?!们|国|方|军|党|校|司|家|公司)|这台设备|本机|这部手机|这台电脑
bool SearchSelf(const View s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == U'我') {
      const View next = s.substr(i + 1);
      if (MatchAny(next, {U"们", U"国", U"方", U"军", U"党", U"校", U"司", U"家", U"公司"}) == 0) {
        return true;
      }
    }
  }
  return Contains(s, U"这台设备") || Contains(s, U"本机") || Contains(s, U"这部手机") || Contains(s, U"这台电脑");
}

// (了吗|的是谁|是谁|过吗|没有)$
bool QuestionTail(const View s) {
  for (const View t : {View(U"了吗"), View(U"的是谁"), View(U"是谁"), View(U"过吗"), View(U"没有")}) {
    if (s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0) {
      return true;
    }
  }
  return false;
}

bool AnyIn(const std::vector<Text>& list, const View needle) {
  return std::any_of(list.begin(), list.end(), [&](const Text& w) { return View(w) == needle; });
}

bool AnyContained(const std::vector<Text>& list, const View msg) {
  return std::any_of(list.begin(), list.end(), [&](const Text& w) { return Contains(msg, w); });
}

bool AnyPrefix(const std::vector<Text>& list, const View s) {
  return std::any_of(list.begin(), list.end(), [&](const Text& w) { return StartsWith(s, w); });
}

enum class Hit { None, Phrase, VerbObject };

Hit ZhHitKind(const ZhEntry& e, const View msg) {
  if (AnyContained(e.phrases, msg)) {
    return Hit::Phrase;
  }
  std::vector<size_t> verb_pos, obj_pos;
  for (const Text& v : e.verbs) {
    if (const size_t p = msg.find(v); p != View::npos) {
      verb_pos.push_back(p);
    }
  }
  for (const Text& o : e.objects) {
    if (const size_t p = msg.find(o); p != View::npos) {
      obj_pos.push_back(p);
    }
  }
  for (const size_t vp : verb_pos) {
    for (const size_t op : obj_pos) {
      if ((vp > op ? vp - op : op - vp) <= kZhMaxDistance) {
        return Hit::VerbObject;
      }
    }
  }
  return Hit::None;
}

size_t RFindEnd(const View msg, const View w) { return msg.rfind(w) + w.size(); }

bool ZhFormOk(const View msg, const ZhEntry& e, const Hit hit) {
  std::vector<Text> starters;
  View rest = msg;
  while (const size_t n = MatchStarter(rest)) {
    starters.emplace_back(rest.substr(0, n));
    rest = rest.substr(n);
  }
  if (!starters.empty()) {
    if (std::any_of(starters.begin(), starters.end(), [&](const Text& st) { return AnyIn(e.objects, st); })) {
      size_t end = 0; // 给/和/跟 + person + verb
      bool found = false;
      for (const auto* list : {&e.verbs, &e.phrases}) {
        for (const Text& w : *list) {
          if (Contains(msg, w)) {
            end = std::max(end, RFindEnd(msg, w));
            found = true;
          }
        }
      }
      if (found && msg.size() - end <= kZhPrepMaxTail && !QuestionTail(msg)) {
        return true;
      }
    }
    const size_t pron = MatchPronoun(rest);
    for (const View r : {rest, rest.substr(pron)}) {
      if (StartsWith(r, U"我") || MatchAppRef(r) || AnyPrefix(e.verbs, r) || AnyPrefix(e.objects, r) ||
          AnyPrefix(e.phrases, r)) {
        return true;
      }
    }
  }
  if (!MatchOpinion(msg) && SearchSelf(rest)) {
    return true;
  }
  if (MatchAppRef(rest)) {
    return true;
  }
  if (hit == Hit::Phrase) {
    if (MatchHowTo(rest)) {
      return true;
    }
    if (e.settings_read && msg.size() <= kZhBareMaxChars && AnyPrefix(e.phrases, msg)) {
      return true;
    }
    if (AnyPrefix(e.verbs, msg)) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ClassifyChinese(const Text& decoded, const std::vector<std::string>& declared) {
  Text msg;
  for (const char32_t c : decoded) {
    if (!IsSpace(c)) {
      msg.push_back(Lower(c));
    }
  }
  if (msg.empty() || msg.size() > kZhMaxChars) {
    return std::nullopt;
  }
  std::vector<const ZhEntry*> entries;
  for (const std::string& name : declared) {
    for (const ZhEntry& e : ZhTable()) {
      if (e.tool == name) {
        entries.push_back(&e);
        break;
      }
    }
  }
  for (const Hit want : {Hit::Phrase, Hit::VerbObject}) {
    for (const ZhEntry* e : entries) {
      const Hit kind = ZhHitKind(*e, msg);
      if (kind == want && ZhFormOk(msg, *e, kind)) {
        return std::string(e->tool);
      }
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------
// English: lowercase words (letters, digits, apostrophes); phrases match on word boundaries.
// ---------------------------------------------------------------------------------------------------------

using Words = std::vector<std::string>;

Words Split(std::string_view s) {
  Words out;
  std::string cur;
  const auto flush = [&] {
    if (!cur.empty()) {
      out.push_back(std::move(cur));
      cur.clear();
    }
  };
  for (const char ch : s) {
    const char c = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : ch;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '\'') {
      cur.push_back(c);
    } else {
      flush();
    }
  }
  flush();
  return out;
}

struct EnEntry {
  std::string_view tool;
  bool settings_read = false;
  std::vector<Words> verbs, objects, phrases;
};

const std::vector<EnEntry>& EnTable() {
  static const std::vector<EnEntry> table = [] {
    const auto all = [](const std::vector<std::string_view>& words) {
      std::vector<Words> out;
      for (const auto w : words) {
        out.push_back(Split(w));
      }
      return out;
    };
    std::vector<EnEntry> t;
    for (const AppActionWords& w : AppActionTable()) {
      t.push_back(EnEntry{w.tool, w.settings_read, all(w.en_verbs), all(w.en_objects), all(w.en_phrases)});
    }
    return t;
  }();
  return table;
}

bool WordsAt(const Words& msg, const size_t pos, const Words& p) {
  return !p.empty() && pos + p.size() <= msg.size() && std::equal(p.begin(), p.end(), msg.begin() + static_cast<std::ptrdiff_t>(pos));
}

// First position of `p` in `msg`, or npos.
size_t WordsFind(const Words& msg, const Words& p) {
  for (size_t i = 0; i < msg.size(); ++i) {
    if (WordsAt(msg, i, p)) {
      return i;
    }
  }
  return std::string::npos;
}

bool WordsContain(const Words& msg, const Words& p) { return WordsFind(msg, p) != std::string::npos; }

bool WordsPrefix(const Words& msg, const size_t from, const std::vector<Words>& list) {
  return std::any_of(list.begin(), list.end(), [&](const Words& p) { return WordsAt(msg, from, p); });
}

Hit EnHitKind(const EnEntry& e, const Words& msg) {
  if (std::any_of(e.phrases.begin(), e.phrases.end(), [&](const Words& p) { return WordsContain(msg, p); })) {
    return Hit::Phrase;
  }
  for (const Words& v : e.verbs) {
    const size_t vp = WordsFind(msg, v);
    if (vp == std::string::npos) {
      continue;
    }
    for (const Words& o : e.objects) {
      const size_t op = WordsFind(msg, o);
      if (op != std::string::npos && op >= vp && op - vp <= kEnMaxDistance) {
        return Hit::VerbObject;
      }
    }
  }
  return Hit::None;
}

// Strips leading starters ("please", "can you", ...) and returns the index of the first word after them.
size_t SkipStarters(const Words& msg, bool& had_starter) {
  static const std::vector<Words> starters = {Split("please"),    Split("can you"),      Split("could you"),
                                              Split("would you"), Split("help me"),      Split("i want to"),
                                              Split("i'd like to"), Split("let me"),     Split("let's")};
  size_t pos = 0;
  had_starter = false;
  for (bool again = true; again;) {
    again = false;
    for (const Words& s : starters) {
      if (WordsAt(msg, pos, s)) {
        pos += s.size();
        had_starter = true;
        again = true;
        break;
      }
    }
  }
  return pos;
}

bool EnFormOk(const Words& msg, const EnEntry& e, const Hit hit) {
  bool had_starter = false;
  const size_t rest = SkipStarters(msg, had_starter);
  static const Words kMy = {"my"};

  // (a) imperative
  if (had_starter && (WordsPrefix(msg, rest, e.verbs) || WordsPrefix(msg, rest, e.objects) ||
                      WordsPrefix(msg, rest, e.phrases) || WordsAt(msg, rest, kMy) || WordsAt(msg, rest, Words{"me"}))) {
    return true;
  }
  if (WordsPrefix(msg, 0, e.verbs)) {
    return true;
  }

  // (b) about the user's own app or data; "i think ..." style opinions do not count
  static const std::vector<Words> opinions = {Split("i think"), Split("i heard"), Split("i wonder"),
                                              Split("i feel"),  Split("i believe"), Split("i read")};
  if (!WordsPrefix(msg, 0, opinions)) {
    static const std::vector<Words> own = {Split("my"), Split("i"), Split("this device"), Split("this phone")};
    for (size_t i = rest; i < msg.size(); ++i) {
      if (WordsPrefix(msg, i, own)) {
        return true;
      }
    }
  }

  // (c) how-to
  if (WordsAt(msg, rest, Split("how do i")) || WordsAt(msg, rest, Split("how to")) ||
      WordsAt(msg, rest, Split("how can i"))) {
    return true;
  }

  // A bare settings-read phrase ("supported languages") is a question about the app.
  return hit == Hit::Phrase && e.settings_read && msg.size() <= kEnBareMaxWords && WordsPrefix(msg, 0, e.phrases);
}

std::optional<std::string> ClassifyEnglish(std::string_view message, const std::vector<std::string>& declared) {
  std::string text(message);
  // Curly apostrophes would otherwise split "i'd" in two.
  for (size_t p; (p = text.find("\xE2\x80\x99")) != std::string::npos;) {
    text.replace(p, 3, "'");
  }
  const Words msg = Split(text);
  if (msg.empty() || message.size() > kEnMaxChars) {
    return std::nullopt;
  }
  std::vector<const EnEntry*> entries;
  for (const std::string& name : declared) {
    for (const EnEntry& e : EnTable()) {
      if (e.tool == name) {
        entries.push_back(&e);
        break;
      }
    }
  }
  for (const Hit want : {Hit::Phrase, Hit::VerbObject}) {
    for (const EnEntry* e : entries) {
      const Hit kind = EnHitKind(*e, msg);
      if (kind == want && EnFormOk(msg, *e, kind)) {
        return std::string(e->tool);
      }
    }
  }
  return std::nullopt;
}

} // namespace

std::optional<std::string> ClassifyAppAction(const std::string_view message, const std::vector<std::string>& declared_tools) {
  if (declared_tools.empty()) {
    return std::nullopt;
  }
  const Text decoded = Decode(message);
  if (std::any_of(decoded.begin(), decoded.end(), IsCjk)) {
    return ClassifyChinese(decoded, declared_tools);
  }
  return ClassifyEnglish(message, declared_tools);
}

} // namespace pbr
