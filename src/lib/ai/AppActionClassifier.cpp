#include "lib/ai/AppActionClassifier.h"

#include "lib/ai/AppActionText.h"
#include "lib/ai/AppActionWords.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <initializer_list>

namespace pbr {
namespace {

constexpr size_t kZhMaxChars = 80;      // code points, after whitespace is stripped
constexpr size_t kZhMaxDistance = 12;   // verb/object distance, in code points
constexpr size_t kZhPrepMaxTail = 2;    // after a preposition-style starter (给/和/跟) at most "吧" / "一下" may follow
constexpr size_t kZhBareMaxChars = 10;  // a bare settings-read phrase
constexpr size_t kZhReaderMaxChars = 14; // "看一下设置" style: reading verb first, nothing else
constexpr size_t kZhSearchMaxTail = 10;  // a name after 找一下
constexpr size_t kEnBareCommandMaxWords = 3; // "dark mode"
constexpr size_t kEnMaxChars = 160;
constexpr size_t kEnMaxDistance = 5;    // words from the verb to the object
constexpr size_t kEnBareMaxWords = 5;   // a bare settings-read phrase
constexpr size_t kEnSearchMaxTail = 3;  // words after "people"/"user"/…: a name fits, a relative clause does not

using Text = std::u32string;
using View = std::u32string_view;

Text Decode(std::string_view in, std::vector<size_t>* offsets = nullptr) {
  return app_action_text::DecodeUtf8(in, offsets);
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
  bool bare_command = false;
  bool bare_object = false;
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
      t.push_back(ZhEntry{w.tool, w.settings_read, w.bare_command, w.bare_object, all(w.zh_verbs), all(w.zh_objects), all(w.zh_phrases)});
    }
    return t;
  }();
  return table;
}

// Non-ASCII words are written as UTF-8 narrow literals and decoded here. A char32_t literal with CJK in it
// depends on the compiler reading the source as UTF-8, which MSVC does not do without /utf-8.
Text W(const char* utf8) {
  return Decode(utf8);
}

// First matching prefix, in the given order (regex alternation semantics); returns its length or 0.
size_t MatchAny(const View s, const std::initializer_list<const char*> prefixes) {
  for (const char* utf8 : prefixes) {
    const Text p = W(utf8);
    if (StartsWith(s, p)) {
      return p.size();
    }
  }
  return 0;
}

size_t MatchStarter(const View s) {
  return MatchAny(s, {"请", "帮我", "帮忙", "给我", "给", "替我", "麻烦", "把", "让", "和", "跟", "能不能",
                      "能否", "可不可以", "我要", "我想要", "我想"});
}

size_t MatchPronoun(const View s) {
  return MatchAny(s, {"他", "她", "它", "这个人", "这个", "那个", "这些", "这位", "此人"});
}

bool MatchOpinion(const View s) {
  return MatchAny(s, {"我觉得", "我认为", "我看", "我听说", "我想知道", "我想了解", "我想问", "我想请教", "我好奇",
                      "我感觉"}) > 0;
}

// ^(在|用)?((pp|app)(?![a-z])|这里|这个软件|这个应用|界面|设置里|通讯录)
bool MatchAppRef(const View s) {
  const auto body = [](const View r) {
    for (const View p : {View(U"pp"), View(U"app")}) {
      if (StartsWith(r, p) && !(r.size() > p.size() && r[p.size()] >= 'a' && r[p.size()] <= 'z')) {
        return true;
      }
    }
    return MatchAny(r, {"这里", "这个软件", "这个应用", "界面", "设置里", "通讯录"}) > 0;
  };
  if (!s.empty() && (s[0] == W("在")[0] || s[0] == W("用")[0]) && body(s.substr(1))) {
    return true;
  }
  return body(s);
}

// ^(怎么|如何|怎样|咋)(?!评价|看待|看|理解|解读)
bool MatchHowTo(const View s) {
  const size_t n = MatchAny(s, {"怎么", "如何", "怎样", "咋"});
  return n > 0 && MatchAny(s.substr(n), {"评价", "看待", "看", "理解", "解读"}) == 0;
}

// 我(?!们|国|方|军|党|校|司|家|公司)|这台设备|本机|这部手机|这台电脑
bool SearchSelf(const View s) {
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == W("我")[0]) {
      const View next = s.substr(i + 1);
      if (MatchAny(next, {"们", "国", "方", "军", "党", "校", "司", "家", "公司"}) == 0) {
        return true;
      }
    }
  }
  return Contains(s, W("这台设备")) || Contains(s, W("本机")) || Contains(s, W("这部手机")) || Contains(s, W("这台电脑"));
}

// (了吗|的是谁|是谁|过吗|没有)$
bool QuestionTail(const View s) {
  for (const char* utf8 : {"了吗", "的是谁", "是谁", "过吗", "没有"}) {
    const Text t = W(utf8);
    if (s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0) {
      return true;
    }
  }
  return false;
}

bool AnyIn(const std::vector<Text>& list, const View needle) {
  return std::any_of(list.begin(), list.end(), [&](const Text& w) { return View(w) == needle; });
}

bool AnyPrefix(const std::vector<Text>& list, const View s) {
  return std::any_of(list.begin(), list.end(), [&](const Text& w) { return StartsWith(s, w); });
}

enum class Hit { None, Phrase, VerbObject };

// Spans are [begin, end) in units of the matcher (code points of the normalised message / words), ordered by begin.
using SpanList = std::vector<std::pair<size_t, size_t>>;

struct HitResult {
  Hit kind = Hit::None;
  SpanList spans;
};

void SortSpans(SpanList& spans) { std::sort(spans.begin(), spans.end()); }

HitResult ZhHitKind(const ZhEntry& e, const View msg) {
  for (const Text& w : e.phrases) {
    if (const size_t p = msg.find(w); p != View::npos) {
      return {Hit::Phrase, {{p, p + w.size()}}};
    }
  }
  std::vector<std::pair<size_t, size_t>> verbs, objs; // position, length
  for (const Text& v : e.verbs) {
    if (const size_t p = msg.find(v); p != View::npos) {
      verbs.emplace_back(p, v.size());
    }
  }
  for (const Text& o : e.objects) {
    if (const size_t p = msg.find(o); p != View::npos) {
      objs.emplace_back(p, o.size());
    }
  }
  for (const auto& [vp, vl] : verbs) {
    for (const auto& [op, ol] : objs) {
      if ((vp > op ? vp - op : op - vp) <= kZhMaxDistance) {
        HitResult r{Hit::VerbObject, {{vp, vp + vl}, {op, op + ol}}};
        SortSpans(r.spans);
        return r;
      }
    }
  }
  return {};
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
      if (StartsWith(r, W("我")) || MatchAppRef(r) || AnyPrefix(e.verbs, r) || AnyPrefix(e.objects, r) ||
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
    // "深色模式" is a command, "深色模式对眼睛好吗" is not: at most "吧" / "一下" may follow the phrase.
    if (e.bare_command && std::any_of(e.phrases.begin(), e.phrases.end(), [&](const Text& p) {
          return StartsWith(msg, p) && msg.size() <= p.size() + kZhPrepMaxTail;
        })) {
      return true;
    }
    if (AnyPrefix(e.verbs, msg)) {
      return true;
    }
  }
  // "看一下设置": a short reader sentence that opens with a reading verb needs no "我" or starter.
  if (e.settings_read && msg.size() <= kZhReaderMaxChars && AnyPrefix(e.verbs, msg)) {
    return true;
  }
  return false;
}

// "找一下 Alice" is a directory search, "找一下伊朗核谈判的最新新闻" is not: after a bare 找一下 / 搜一下 at most a name follows.
bool ZhSearchTailOk(const View msg, const HitResult& hit) {
  if (hit.kind != Hit::Phrase) {
    return true;
  }
  const View phrase = msg.substr(hit.spans[0].first, hit.spans[0].second - hit.spans[0].first);
  if (phrase != W("找一下") && phrase != W("搜一下")) {
    return true;
  }
  const View tail = msg.substr(hit.spans[0].second);
  return tail.size() <= kZhSearchMaxTail && tail.find(W("的")[0]) == View::npos;
}

// "通知呢" / "通知？": the object word alone, with at most a question particle behind it. "通知我一下" is a request to
// notify someone, not the setting.
bool IsZhParticle(const char32_t c) {
  return c == W("呢")[0] || c == W("吗")[0] || c == W("吧")[0] || c == W("啊")[0] || c == W("？")[0] || c == '?' ||
         c == W("！")[0] || c == '!';
}

std::optional<size_t> ZhBareObject(const ZhEntry& e, const View msg) {
  size_t end = msg.size();
  while (end > 0 && IsZhParticle(msg[end - 1])) {
    --end;
  }
  const View body = msg.substr(0, end);
  return !body.empty() && AnyIn(e.objects, body) ? std::optional<size_t>(end) : std::nullopt;
}

std::optional<AppActionMatch> ClassifyChinese(const Text& decoded, const std::vector<size_t>& offsets,
                                              const size_t total_bytes, const std::vector<std::string>& declared) {
  Text msg;
  std::vector<size_t> begin, end; // byte range of every code point of `msg` in the original message
  for (size_t i = 0; i < decoded.size(); ++i) {
    if (!IsSpace(decoded[i])) {
      msg.push_back(Lower(decoded[i]));
      begin.push_back(offsets[i]);
      end.push_back(i + 1 < offsets.size() ? offsets[i + 1] : total_bytes);
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
  for (const ZhEntry* e : entries) {
    if (e->bare_object) {
      if (const auto stop = ZhBareObject(*e, msg)) {
        return AppActionMatch{std::string(e->tool), {{begin[0], end[*stop - 1]}}};
      }
    }
  }
  for (const Hit want : {Hit::Phrase, Hit::VerbObject}) {
    for (const ZhEntry* e : entries) {
      const HitResult hit = ZhHitKind(*e, msg);
      if (hit.kind == want && ZhFormOk(msg, *e, hit.kind) && (e->tool != "search_people" || ZhSearchTailOk(msg, hit))) {
        AppActionMatch match{std::string(e->tool), {}};
        for (const auto& [a, b] : hit.spans) {
          match.matched_spans.push_back({begin[a], end[b - 1]});
        }
        return match;
      }
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------
// English: lowercase words (letters, digits, apostrophes); phrases match on word boundaries.
// ---------------------------------------------------------------------------------------------------------

using Words = std::vector<std::string>;

// `spans`, when given, receives the byte range [begin, end) of every word in `s` (shared tokenizer, so the
// planner reads the same words at the same offsets).
Words SplitSpans(std::string_view s, SpanList* spans) {
  Words out;
  for (app_action_text::WordToken& token : app_action_text::TokenizeWords(s)) {
    if (spans) {
      spans->emplace_back(token.begin, token.end);
    }
    out.push_back(std::move(token.word));
  }
  return out;
}

Words Split(std::string_view s) { return SplitSpans(s, nullptr); }

struct EnEntry {
  std::string_view tool;
  bool settings_read = false;
  bool bare_command = false;
  bool bare_object = false;
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
      t.push_back(EnEntry{w.tool, w.settings_read, w.bare_command, w.bare_object, all(w.en_verbs), all(w.en_objects), all(w.en_phrases)});
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

bool WordsPrefix(const Words& msg, const size_t from, const std::vector<Words>& list) {
  return std::any_of(list.begin(), list.end(), [&](const Words& p) { return WordsAt(msg, from, p); });
}

HitResult EnHitKind(const EnEntry& e, const Words& msg) {
  for (const Words& p : e.phrases) {
    if (const size_t at = WordsFind(msg, p); at != std::string::npos) {
      return {Hit::Phrase, {{at, at + p.size()}}};
    }
  }
  for (const Words& v : e.verbs) {
    const size_t vp = WordsFind(msg, v);
    if (vp == std::string::npos) {
      continue;
    }
    for (const Words& o : e.objects) {
      const size_t op = WordsFind(msg, o);
      if (op != std::string::npos && op >= vp && op - vp <= kEnMaxDistance) {
        HitResult r{Hit::VerbObject, {{vp, vp + v.size()}, {op, op + o.size()}}};
        SortSpans(r.spans);
        return r;
      }
    }
  }
  // "look up Ken": with no person word, the verb alone is the hit (the name comes after it).
  if (e.tool == "search_people") {
    if (const size_t at = WordsFind(msg, Words{"look", "up"}); at != std::string::npos) {
      return {Hit::Phrase, {{at, at + 2}}};
    }
  }
  return {};
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

  // (b) about the user's own app or data. "my" has to qualify one of the tool's words ("my contacts",
  // "is my account secure") and "I" has to open the question ("am I registered"). A "my" or "I"
  // anywhere else is someone's story ("... for my thesis", "my friend said ..."), and a hit there would
  // now skip brief_AI and could run a setter tool.
  static const std::vector<Words> device = {Split("this device"), Split("this phone")};
  // Not "can I" / "could I": those open questions about anything ("can I mute notifications on WhatsApp?").
  static const std::vector<Words> i_openers = {Split("am i"), Split("do i"), Split("did i"), Split("have i")};
  if (WordsPrefix(msg, rest, i_openers)) {
    return true;
  }
  for (size_t i = rest; i < msg.size(); ++i) {
    if (WordsPrefix(msg, i, device)) {
      return true;
    }
    if (msg[i] == "my" && (WordsPrefix(msg, i, e.phrases) || WordsPrefix(msg, i, e.objects) || WordsPrefix(msg, i + 1, e.objects) ||
                           WordsPrefix(msg, i + 1, e.phrases) || WordsPrefix(msg, i + 2, e.objects))) {
      return true;
    }
  }

  // (c) how-to
  if (WordsAt(msg, rest, Split("how do i")) || WordsAt(msg, rest, Split("how to")) ||
      WordsAt(msg, rest, Split("how can i"))) {
    return true;
  }

  // A bare settings-read phrase ("supported languages") is a question about the app.
  if (hit != Hit::Phrase || !WordsPrefix(msg, 0, e.phrases)) {
    return false;
  }
  return (e.settings_read && msg.size() <= kEnBareMaxWords) || (e.bare_command && msg.size() <= kEnBareCommandMaxWords);
}

// "Find people who survived the Titanic" asks about the world, not the directory: after the person word a
// directory search has at most a name.
bool EnSearchTailOk(const Words& msg, const EnEntry& e) {
  for (size_t i = msg.size(); i-- > 0;) {
    for (const Words& object : e.objects) {
      if (WordsAt(msg, i, object)) {
        return msg.size() - (i + object.size()) <= kEnSearchMaxTail;
      }
    }
  }
  // "look up Ken": the verb itself is the anchor when no person word is there.
  if (const size_t at = WordsFind(msg, Words{"look", "up"}); at != std::string::npos) {
    return msg.size() - (at + 2) <= kEnSearchMaxTail;
  }
  return true;
}

std::optional<AppActionMatch> ClassifyEnglish(std::string_view message, const std::vector<std::string>& declared) {
  SpanList word_bytes;
  const Words msg = SplitSpans(message, &word_bytes);
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
  for (const EnEntry* e : entries) {
    // "Notifications?": the object word alone (the tokenizer already dropped the mark).
    if (e->bare_object && std::any_of(e->objects.begin(), e->objects.end(), [&](const Words& o) { return o == msg; })) {
      return AppActionMatch{std::string(e->tool), {{word_bytes.front().first, word_bytes.back().second}}};
    }
  }
  for (const Hit want : {Hit::Phrase, Hit::VerbObject}) {
    for (const EnEntry* e : entries) {
      const HitResult hit = EnHitKind(*e, msg);
      if (hit.kind == want && EnFormOk(msg, *e, hit.kind) && (e->tool != "search_people" || EnSearchTailOk(msg, *e))) {
        AppActionMatch match{std::string(e->tool), {}};
        for (const auto& [a, b] : hit.spans) {
          match.matched_spans.push_back({word_bytes[a].first, word_bytes[b - 1].second});
        }
        return match;
      }
    }
  }
  return std::nullopt;
}

} // namespace

std::optional<AppActionMatch> ClassifyAppActionMatch(const std::string_view message,
                                                     const std::vector<std::string>& declared_tools) {
  if (declared_tools.empty()) {
    return std::nullopt;
  }
  // The add-contact intent still uses the shared wordlist, but this client offers search
  // and a user-selected Add action instead of a mutating AI tool.
  auto capabilities = declared_tools;
  const bool search_before_add =
      std::find(capabilities.begin(), capabilities.end(), "search_people") != capabilities.end() &&
      std::find(capabilities.begin(), capabilities.end(), "add_contact") == capabilities.end();
  if (search_before_add) {
    capabilities.push_back("add_contact");
  }
  std::vector<size_t> offsets;
  const Text decoded = Decode(message, &offsets);
  auto match = std::any_of(decoded.begin(), decoded.end(), IsCjk)
                   ? ClassifyChinese(decoded, offsets, message.size(), capabilities)
                   : ClassifyEnglish(message, capabilities);
  if (match && search_before_add && match->tool == "add_contact") {
    match->tool = "search_people";
  }
  return match;
}

std::optional<std::string> ClassifyAppAction(const std::string_view message, const std::vector<std::string>& declared_tools) {
  if (auto match = ClassifyAppActionMatch(message, declared_tools)) {
    return std::move(match->tool);
  }
  return std::nullopt;
}

bool ContainsAppActionHit(const std::string_view text) {
  const Text decoded = Decode(text);
  if (std::any_of(decoded.begin(), decoded.end(), IsCjk)) {
    Text msg;
    for (const char32_t c : decoded) {
      if (!IsSpace(c)) {
        msg.push_back(Lower(c));
      }
    }
    return std::any_of(ZhTable().begin(), ZhTable().end(),
                       [&](const ZhEntry& e) { return ZhHitKind(e, msg).kind != Hit::None; });
  }
  const Words msg = Split(text);
  return std::any_of(EnTable().begin(), EnTable().end(),
                     [&](const EnEntry& e) { return EnHitKind(e, msg).kind != Hit::None; });
}

} // namespace pbr
