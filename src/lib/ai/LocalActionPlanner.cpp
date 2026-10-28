#include "lib/ai/LocalActionPlanner.h"

#include "lib/ai/AppActionText.h"
#include "lib/ai/AppActionWords.h"
#include "lib/ai/LocalActionWords.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>

namespace pbr {
namespace {

using U32 = std::u32string;
constexpr size_t kNpos = std::string::npos;

// ---------------------------------------------------------------------------------------------------------
// Text helpers. Offsets are bytes of the original UTF-8 text; ASCII lowercasing keeps them.
// ---------------------------------------------------------------------------------------------------------

std::string LowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }
  return s;
}

bool IsWordChar(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool IsAsciiPhrase(const std::string_view w) { return !w.empty() && static_cast<unsigned char>(w.front()) < 0x80; }

U32 ToU32(const std::string_view in) { return app_action_text::DecodeUtf8(in); }

size_t CodePoints(const std::string_view s) { return ToU32(s).size(); }

std::vector<size_t> FindAll(const U32& hay, const U32& needle) {
  std::vector<size_t> out;
  if (needle.empty()) {
    return out;
  }
  for (size_t p = hay.find(needle); p != U32::npos; p = hay.find(needle, p + 1)) {
    out.push_back(p);
  }
  return out;
}

/** Whitespace and punctuation that never belong to a parameter at its edges. */
constexpr auto kEdgeStrings = std::to_array<std::string_view>(
    {" ", "\t", "\r", "\n", "\xE3\x80\x80", "\xC2\xA0", ":", "\xEF\xBC\x9A", "?", "\xEF\xBC\x9F", "!", "\xEF\xBC\x81",
     ".", "\xE3\x80\x82", ",", "\xEF\xBC\x8C", "\"", "\xE2\x80\x9C", "\xE2\x80\x9D", "\xE3\x80\x8C", "\xE3\x80\x8D",
     "\xE3\x80\x81", "~"});

bool StartsWith(const std::string& s, const std::string_view p) { return s.compare(0, p.size(), p) == 0; }
bool EndsWith(const std::string& s, const std::string_view p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string StripEdges(std::string s) {
  for (bool again = true; again;) {
    again = false;
    for (const std::string_view e : kEdgeStrings) {
      if (StartsWith(s, e)) {
        s.erase(0, e.size());
        again = true;
      }
      if (EndsWith(s, e)) {
        s.erase(s.size() - e.size());
        again = true;
      }
    }
  }
  return s;
}

bool IsSingleCjk(const std::string_view w) { return w.size() == 3 && static_cast<unsigned char>(w[0]) >= 0x80; }

/** Longest filler at the head of `s` (case-insensitive; an ASCII word must end at a word boundary). */
std::string_view HeadFiller(const std::string& s, const std::span<const std::string_view> words) {
  const std::string lower = LowerAscii(s);
  std::string_view best;
  for (const std::string_view w : words) {
    if (w.size() > best.size() && StartsWith(lower, w) &&
        !(IsWordChar(w.back()) && w.size() < lower.size() && IsWordChar(lower[w.size()]))) {
      best = w;
    }
  }
  return best;
}

std::string_view TailFiller(const std::string& s, const std::span<const std::string_view> words) {
  const std::string lower = LowerAscii(s);
  std::string_view best;
  for (const std::string_view w : words) {
    if (w.size() > best.size() && EndsWith(lower, w) &&
        !(IsWordChar(w.front()) && lower.size() > w.size() && IsWordChar(lower[lower.size() - w.size() - 1]))) {
      best = w;
    }
  }
  return best;
}

/**
 * Strips fillers from the head: at most two, and the second only if it is not a single Chinese character
 * ("改昵称为成龙" keeps 成龙). A strip that would empty the text is not done.
 */
std::string StripHead(std::string s, const std::span<const std::string_view> fillers) {
  int strips = 0;
  for (;;) {
    s = StripEdges(std::move(s));
    const std::string_view f = HeadFiller(s, fillers);
    if (f.empty() || strips >= 2 || (strips >= 1 && IsSingleCjk(f))) {
      break;
    }
    const std::string rest = StripEdges(s.substr(f.size()));
    if (rest.empty()) {
      break;
    }
    s = rest;
    ++strips;
  }
  return s;
}

/** Strips every lead word ("我想和", "Add") from the head. */
std::string StripLead(std::string s) {
  for (;;) {
    s = StripEdges(std::move(s));
    const std::string_view f = HeadFiller(s, kLeadWords);
    if (f.empty()) {
      break;
    }
    const std::string rest = StripEdges(s.substr(f.size()));
    if (rest.empty()) {
      break;
    }
    s = rest;
  }
  return s;
}

std::string StripTail(std::string s, const std::span<const std::string_view> fillers) {
  for (;;) {
    s = StripEdges(std::move(s));
    const std::string_view f = TailFiller(s, fillers);
    if (f.empty()) {
      break;
    }
    const std::string rest = StripEdges(s.substr(0, s.size() - f.size()));
    if (rest.empty()) {
      break;
    }
    s = rest;
  }
  return s;
}

// --- word search --------------------------------------------------------------------------------------------

using Token = app_action_text::WordToken;

/** ASCII words (letters, digits, apostrophes): the classifier's tokenizer, so offsets agree. */
std::vector<Token> Tokenize(const std::string& lower) { return app_action_text::TokenizeWords(lower); }

std::vector<std::string> PhraseWords(const std::string_view phrase) {
  std::vector<std::string> out;
  for (Token& t : Tokenize(std::string(phrase))) {
    out.push_back(std::move(t.word));
  }
  return out;
}

/** Index of the first token run equal to `words`, or kNpos. */
size_t FindRun(const std::vector<Token>& toks, const std::vector<std::string>& words, const size_t from = 0) {
  if (words.empty()) {
    return kNpos;
  }
  for (size_t i = from; i + words.size() <= toks.size(); ++i) {
    bool same = true;
    for (size_t k = 0; k < words.size(); ++k) {
      if (toks[i + k].word != words[k]) {
        same = false;
        break;
      }
    }
    if (same) {
      return i;
    }
  }
  return kNpos;
}

/** One lowercased sentence, searchable by Chinese substring or English whole word. */
struct Haystack {
  explicit Haystack(const std::string& text) : lower(LowerAscii(text)), tokens(Tokenize(lower)) {}

  bool Has(const std::string_view word) const {
    if (IsAsciiPhrase(word)) {
      return FindRun(tokens, PhraseWords(word)) != kNpos;
    }
    return lower.find(word) != kNpos;
  }
  bool HasAny(const std::span<const std::string_view> words) const {
    return std::any_of(words.begin(), words.end(), [&](const std::string_view w) { return Has(w); });
  }

  std::string lower;
  std::vector<Token> tokens;
};

struct Stop {
  size_t pos = kNpos;
  size_t len = 0;
};

/** The earliest stop word / mark in `s`. */
Stop FindStop(const std::string& s) {
  const Haystack h(s);
  Stop best;
  const auto consider = [&](const size_t pos, const size_t len) {
    if (pos < best.pos) {
      best = {pos, len};
    }
  };
  for (const std::string_view w : kStopWordsZh) {
    consider(h.lower.find(w), w.size());
  }
  for (const std::string_view w : kStopChars) {
    consider(h.lower.find(w), w.size());
  }
  for (const std::string_view w : kStopWordsEn) {
    for (const Token& t : h.tokens) {
      if (t.word == w) {
        consider(t.begin, t.end - t.begin);
        break;
      }
    }
  }
  return best;
}

// ---------------------------------------------------------------------------------------------------------
// Sentence shape
// ---------------------------------------------------------------------------------------------------------

bool IsQuestion(const std::string& text, const bool settings_read) {
  const Haystack h(text);
  if (h.HasAny(kQuestionWordsZh) || h.HasAny(kQuestionWordsEn)) {
    return true;
  }
  return !settings_read && (h.HasAny(kWhatIsWordsZh) || h.HasAny(kWhatIsWordsEn));
}

/** Erases every whole-word / substring occurrence of `w` from `s` (a space in its place); true if any. */
bool EraseWord(std::string& s, const std::string_view w) {
  bool erased = false;
  for (size_t p = s.find(w); p != kNpos; p = s.find(w, p + 1)) {
    if (IsAsciiPhrase(w)) {
      const size_t e = p + w.size();
      if ((p > 0 && IsWordChar(s[p - 1])) || (e < s.size() && IsWordChar(s[e]))) {
        continue;
      }
    }
    s.replace(p, w.size(), " ");
    erased = true;
  }
  return erased;
}

/**
 * A cancellation is a short answer that is nothing but cancel words, particles (吧 / 了 / 呢 / 啊) and
 * punctuation: "算了，不改了吧" cancels, "不用了，叫Tom" and "cancel it, use Tom" carry a value and do not.
 */
bool IsCancel(const std::string& answer) {
  const std::string trimmed = StripEdges(answer);
  if (trimmed.empty() || CodePoints(trimmed) > kMaxCancelChars) {
    return false;
  }
  std::string rest = LowerAscii(trimmed);
  bool hit = false;
  for (const std::string_view w : kCancelWords) {
    hit = EraseWord(rest, w) || hit;
  }
  if (!hit) {
    return false;
  }
  for (const std::string_view w : kCancelParticles) {
    EraseWord(rest, w);
  }
  for (const std::string_view w : kEdgeStrings) { // punctuation, wherever it is, becomes a space
    for (size_t p = rest.find(w); p != kNpos && w != " "; p = rest.find(w, p + 1)) {
      rest.replace(p, w.size(), " ");
    }
  }
  for (const std::string_view w : kCancelFillerWordsEn) {
    EraseWord(rest, w);
  }
  return std::all_of(rest.begin(), rest.end(), [](const char c) { return c == ' '; });
}

const AppActionWords* WordsFor(const std::string& tool) {
  for (const AppActionWords& w : AppActionTable()) {
    if (w.tool == tool) {
      return &w;
    }
  }
  return nullptr;
}

/** The parts of the sentence around the hit. */
struct Parts {
  size_t first_begin = 0;
  size_t last_end = 0;
  std::string region; // the clause with the hit, between stop words: what the booleans / enums are read from
  std::string rest_full; // everything after the hit
  std::string rest;      // ... up to the first stop word
  bool more = false;     // a second action follows the stop word
  bool had_stop = false;
};

Parts SplitSentence(const std::string& text, const std::vector<AppActionSpan>& spans) {
  Parts p;
  p.first_begin = text.size();
  for (const AppActionSpan& s : spans) {
    if (s.begin < s.end && s.end <= text.size()) {
      p.first_begin = std::min(p.first_begin, s.begin);
      p.last_end = std::max(p.last_end, s.end);
    }
  }
  if (p.last_end == 0) {
    p.first_begin = 0;
  }
  const std::string prefix = text.substr(0, p.first_begin);
  size_t seg_begin = 0;
  for (Stop s = FindStop(prefix); s.pos != kNpos; s = FindStop(prefix.substr(seg_begin))) {
    seg_begin += s.pos + s.len;
  }
  p.rest_full = text.substr(p.last_end);
  const Stop stop = FindStop(p.rest_full);
  p.rest = stop.pos == kNpos ? p.rest_full : p.rest_full.substr(0, stop.pos);
  p.had_stop = stop.pos != kNpos;
  p.more = stop.pos != kNpos && !StripEdges(p.rest_full.substr(stop.pos + stop.len)).empty();
  p.region = text.substr(seg_begin, p.last_end + p.rest.size() - seg_begin);
  return p;
}

// ---------------------------------------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------------------------------------

bool IsPeopleTool(const std::string& tool) { return tool == "search_people" || tool == "list_contacts"; }

LocalPlan MakePlan(const std::string& tool, Object args, const std::string& text, const bool only_first) {
  TurnPlan plan;
  plan.source = TurnPlanSource::Planner;
  plan.tools.push_back(PlannedToolCall{tool, std::move(args)});
  plan.user_request = text;
  if (IsPeopleTool(tool)) {
    plan.response_goal = ResponseGoal::PeopleDiscovery;
    plan.render_mode = RenderMode::PeopleList;
  } else {
    plan.response_goal = ResponseGoal::General;
    plan.render_mode = RenderMode::Blocks;
  }
  if (only_first) {
    plan.synthesis_hints = std::string(kHintOnlyFirstAction);
  }
  return plan;
}

LocalPlan MakeTextPlan(const std::string& tool, const char* key, const std::string& value, const std::string& text,
                       const bool only_first) {
  Object args;
  args.set(key, value);
  return MakePlan(tool, std::move(args), text, only_first);
}

LocalPlan MakeBoolPlan(const std::string& tool, const char* key, const bool value, const std::string& text,
                       const bool only_first) {
  Object args;
  args.set(key, value);
  return MakePlan(tool, std::move(args), text, only_first);
}

LocalAsk Ask(const std::string& tool, const char* missing, const char* key, std::string carry = {}) {
  return LocalAsk{tool, missing, key, std::move(carry)};
}

enum class ToolClass { NoArg, Probe, Query, Nickname, Bool, Appearance, Language, Policy, Mesh, Contact, Unsupported };

ToolClass ClassOf(const std::string& tool) {
  static constexpr auto kNoArg = std::to_array<std::string_view>(
      {"get_profile_identity", "get_preferences", "get_security_status", "get_network_settings", "get_reachability",
       "get_llm_settings", "get_integrations", "list_locales", "list_conversations", "reset_tool_permissions"});
  static constexpr auto kBool = std::to_array<std::string_view>(
      {"set_notifications", "set_reduce_transparency", "set_auto_renew_registration", "set_call_diagnostics",
       "set_node_enabled"});
  if (std::find(kNoArg.begin(), kNoArg.end(), tool) != kNoArg.end()) {
    return ToolClass::NoArg;
  }
  if (std::find(kBool.begin(), kBool.end(), tool) != kBool.end()) {
    return ToolClass::Bool;
  }
  if (tool == "probe_reachability") {
    return ToolClass::Probe;
  }
  if (tool == "search_people" || tool == "list_contacts") {
    return ToolClass::Query;
  }
  if (tool == "update_profile_nickname" || tool == "register_user") {
    return ToolClass::Nickname;
  }
  if (tool == "set_appearance") {
    return ToolClass::Appearance;
  }
  if (tool == "set_language") {
    return ToolClass::Language;
  }
  if (tool == "set_group_invite_policy" || tool == "set_call_invite_policy") {
    return ToolClass::Policy;
  }
  if (tool == "set_mesh_capabilities") {
    return ToolClass::Mesh;
  }
  if (tool == "open_conversation" || tool == "start_conversation") {
    return ToolClass::Contact;
  }
  return ToolClass::Unsupported;
}

// ---------------------------------------------------------------------------------------------------------
// On / off
// ---------------------------------------------------------------------------------------------------------

struct Direction {
  bool on = false;
  bool off = false;
  bool negated = false;
};

bool IsUnclear(const Direction& d) { return d.negated || (d.on == d.off); }

/**
 * Looks for on / off in `region`. 开 / 关 alone count only next to an object word or at the sentence end (in an
 * answer, also at its start). A negation word shortly before a direction word makes the whole thing unclear.
 */
Direction ScanDirection(const std::string& region, const std::vector<std::string>& object_words, const bool answer_mode) {
  Direction d;
  const Haystack h(region);
  const U32 r = ToU32(h.lower);
  std::vector<char> covered(r.size(), 0), not_negation(r.size(), 0);
  const auto mark = [&](std::vector<char>& v, const std::span<const std::string_view> words) {
    for (const std::string_view w : words) {
      const U32 u = ToU32(w);
      for (const size_t p : FindAll(r, u)) {
        std::fill(v.begin() + static_cast<std::ptrdiff_t>(p), v.begin() + static_cast<std::ptrdiff_t>(p + u.size()), char{1});
      }
    }
  };
  mark(covered, kBoolMaskedWords);
  mark(not_negation, kNegationMaskedWords);

  std::vector<std::pair<size_t, bool>> found; // code point position, on?
  const auto scan_multi = [&](const std::span<const std::string_view> words, const bool on) {
    for (const std::string_view w : words) {
      const U32 u = ToU32(w);
      for (const size_t p : FindAll(r, u)) {
        if (std::any_of(covered.begin() + static_cast<std::ptrdiff_t>(p),
                        covered.begin() + static_cast<std::ptrdiff_t>(p + u.size()), [](const char c) { return c != 0; })) {
          continue;
        }
        std::fill(covered.begin() + static_cast<std::ptrdiff_t>(p), covered.begin() + static_cast<std::ptrdiff_t>(p + u.size()), char{1});
        found.emplace_back(p, on);
      }
    }
  };
  scan_multi(kBoolOnWordsZh, true);
  scan_multi(kBoolOffWordsZh, false);

  std::vector<std::pair<size_t, size_t>> objects;
  for (const std::string& o : object_words) {
    const U32 u = ToU32(o);
    for (const size_t p : FindAll(r, u)) {
      objects.emplace_back(p, p + u.size());
    }
  }
  std::vector<U32> particles;
  for (const std::string_view p : kTrailingParticles) {
    particles.push_back(ToU32(p));
  }
  const auto at_end = [&](size_t j) {
    for (bool again = true; again && j < r.size();) {
      again = false;
      for (const U32& p : particles) {
        if (r.compare(j, p.size(), p) == 0) {
          j += p.size();
          again = true;
          break;
        }
      }
    }
    return j >= r.size();
  };
  const auto at_start = [&](const size_t i) {
    return std::all_of(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(i), [](const char32_t c) { return c == U' '; });
  };
  const char32_t single_on = ToU32(kBoolSingleOn)[0];
  const char32_t single_off = ToU32(kBoolSingleOff)[0];
  for (size_t i = 0; i < r.size(); ++i) {
    if (covered[i] || (r[i] != single_on && r[i] != single_off)) {
      continue;
    }
    const bool next_to_object = std::any_of(objects.begin(), objects.end(), [&](const auto& o) {
      return o.first == i + 1 || o.second == i;
    });
    if (next_to_object || at_end(i + 1) || (answer_mode && at_start(i))) {
      found.emplace_back(i, r[i] == single_on);
    }
  }
  for (const auto& [pos, on] : found) {
    (on ? d.on : d.off) = true;
    const size_t from = pos >= kNegationWindowChars ? pos - kNegationWindowChars : 0;
    for (const std::string_view n : kNegationWordsZh) {
      const U32 u = ToU32(n);
      for (const size_t q : FindAll(r, u)) {
        if (q >= from && q < pos && !not_negation[q]) {
          d.negated = true;
        }
      }
    }
  }

  // English: words.
  const auto words_at = [&](const size_t i, const std::span<const std::string_view> list) -> size_t {
    for (const std::string_view w : list) {
      if (!IsAsciiPhrase(w)) {
        continue;
      }
      const std::vector<std::string> ws = PhraseWords(w);
      if (!ws.empty() && i + ws.size() <= h.tokens.size() &&
          std::equal(ws.begin(), ws.end(), h.tokens.begin() + static_cast<std::ptrdiff_t>(i),
                     [](const std::string& a, const Token& t) { return a == t.word; })) {
        return ws.size();
      }
    }
    return 0;
  };
  for (size_t i = 0; i < h.tokens.size(); ++i) {
    const bool on = words_at(i, kBoolOnWordsEn) > 0;
    const bool off = !on && words_at(i, kBoolOffWordsEn) > 0;
    if (!on && !off) {
      continue;
    }
    (on ? d.on : d.off) = true;
    for (size_t k = i >= kNegationWindowWords ? i - kNegationWindowWords : 0; k < i; ++k) {
      if (std::find(kNegationWordsEn.begin(), kNegationWordsEn.end(), h.tokens[k].word) != kNegationWordsEn.end()) {
        d.negated = true;
      }
    }
  }
  return d;
}

std::vector<std::string> ObjectWordsZh(const std::string& tool) {
  std::vector<std::string> out;
  if (const AppActionWords* w = WordsFor(tool)) {
    for (const std::string_view o : w->zh_objects) {
      out.emplace_back(o);
    }
  }
  if (tool == "set_mesh_capabilities") {
    for (const auto list : {std::span<const std::string_view>(kCapMediaRelayWords),
                            std::span<const std::string_view>(kCapPreferContactsWords),
                            std::span<const std::string_view>(kCapDirectWords)}) {
      for (const std::string_view w : list) {
        out.emplace_back(w);
      }
    }
  }
  return out;
}

enum class Tri { Unclear, False, True };

/** true / false / unclear for a boolean tool. `region` is the clause with the hit, or the whole answer. */
Tri ParseBool(const std::string& tool, const std::string& region, const bool answer_mode) {
  const Direction d = ScanDirection(region, ObjectWordsZh(tool), answer_mode);
  const Haystack h(region);
  bool value = false;
  if (tool == "set_reduce_transparency" && !answer_mode) {
    // "turn off the blur" asks for less transparency: enabled. "reduce transparency" names the setting itself.
    if (h.HasAny(kReduceWords)) {
      if (d.negated || (d.on && d.off)) {
        return Tri::Unclear;
      }
      return d.off ? Tri::False : Tri::True;
    }
    if (IsUnclear(d)) {
      return Tri::Unclear;
    }
    return d.off ? Tri::True : Tri::False;
  }
  if (IsUnclear(d)) {
    return Tri::Unclear;
  }
  value = d.on;
  return value ? Tri::True : Tri::False;
}

// ---------------------------------------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------------------------------------

std::string ParseAppearance(const std::string& region, const bool answer_mode) {
  const Haystack h(region);
  const bool dark = h.HasAny(kAppearanceDarkWords);
  const bool light = h.HasAny(kAppearanceLightWords);
  const bool system = h.HasAny(kAppearanceSystemWords);
  if (static_cast<int>(dark) + static_cast<int>(light) + static_cast<int>(system) != 1) {
    return {};
  }
  const Direction d = ScanDirection(region, {"深色模式", "浅色模式", "夜间模式", "暗色", "外观"}, answer_mode);
  const bool flip = (d.off || d.negated) && !d.on;
  if (system) {
    return flip ? std::string() : "system";
  }
  return (dark != flip) ? "dark" : "light";
}

std::string ParsePolicy(const std::string& region) {
  const Haystack h(region);
  const bool block = h.HasAny(kPolicyBlockWords);
  const bool strangers = h.HasAny(kPolicyStrangerWords);
  const bool anyone = h.HasAny(kPolicyAnyoneWords);
  const bool contacts = h.HasAny(kPolicyContactsWords);
  if (h.HasAny(kPolicyNobodyWords)) {
    return "nobody";
  }
  if (block) {
    if (anyone && !contacts) {
      return "nobody";
    }
    if (strangers && !anyone) {
      return "contacts_only";
    }
    return {};
  }
  if (anyone == contacts) {
    return {};
  }
  return anyone ? "everyone" : "contacts_only";
}

std::string ParseLanguage(const std::string& region, const LocalPlanContext& ctx) {
  const Haystack h(region);
  std::vector<std::pair<std::string, std::string>> locales = ctx.locales;
  if (locales.empty()) {
    locales = {{"zh-Hans", "简体中文"}, {"en", "English"}};
  }
  std::vector<std::string> tags;
  const auto add = [&](const std::string& tag) {
    if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
      tags.push_back(tag);
    }
  };
  for (const auto& [tag, label] : locales) {
    if ((!label.empty() && h.Has(LowerAscii(label))) || h.Has(LowerAscii(tag))) {
      add(tag);
    }
  }
  const auto pick = [&](const char* prefix, const bool traditional) -> std::string {
    std::string first, wanted;
    for (const auto& [tag, label] : locales) {
      const std::string t = LowerAscii(tag);
      if (t.rfind(prefix, 0) != 0) {
        continue;
      }
      if (first.empty()) {
        first = tag;
      }
      const bool is_trad = t.find("hant") != kNpos || t.find("tw") != kNpos || t.find("hk") != kNpos;
      if (is_trad == traditional && wanted.empty()) {
        wanted = tag;
      }
    }
    return wanted.empty() ? first : wanted;
  };
  if (h.HasAny(kLocaleChineseWords) || h.HasAny(kLocaleTraditionalWords)) {
    if (const std::string tag = pick("zh", h.HasAny(kLocaleTraditionalWords)); !tag.empty()) {
      add(tag);
    }
  }
  if (h.HasAny(kLocaleEnglishWords)) {
    if (const std::string tag = pick("en", false); !tag.empty()) {
      add(tag);
    }
  }
  return tags.size() == 1 ? tags.front() : std::string();
}

/** Capability flags named in the sentence, in a fixed order. */
std::vector<std::string> FindCapabilities(const std::string& region) {
  Haystack h(region);
  std::vector<std::string> caps;
  const bool media = h.HasAny(kCapMediaRelayWords);
  if (media) {
    caps.push_back("media_relay");
  }
  // "媒体中继" must not also count as "中继".
  std::string plain = h.lower;
  for (const std::string_view w : kCapMediaRelayWords) {
    for (size_t p; (p = plain.find(w)) != kNpos;) {
      plain.replace(p, w.size(), std::string(w.size(), ' '));
    }
  }
  const Haystack without_media(plain);
  if (without_media.HasAny(kCapRelayWords)) {
    caps.push_back("circuit_relay");
  }
  if (h.HasAny(kCapDhtWords)) {
    caps.push_back("dht");
  }
  if (h.HasAny(kCapPreferContactsWords)) {
    caps.push_back("prefer_contacts_for_routing");
  }
  if (h.HasAny(kCapDirectWords)) {
    caps.push_back("direct_connections");
  }
  return caps;
}

/** The audience of direct_connections named in `region`, or "". */
std::string ParseAudience(const std::string& region) {
  const Haystack h(region);
  if (h.HasAny(kAudienceNobodyWords)) {
    return "nobody";
  }
  if (h.HasAny(kAudienceFriendlyWords)) {
    return "friendly";
  }
  if (h.HasAny(kAudienceContactsWords)) {
    return "contacts";
  }
  if (h.HasAny(kAudienceEveryoneWords)) {
    return "everyone";
  }
  return {};
}

LocalPlan MeshPlan(const std::string& cap, const bool enabled, const std::string& text, const bool only_first,
                   const std::string& region = {}) {
  if (cap == "direct_connections") {
    // The tool takes an audience, not a flag: "off" is unambiguous (always relay); "on" needs to know for whom.
    if (!enabled) {
      return MakeTextPlan("set_mesh_capabilities", "direct_connections", "nobody", text, only_first);
    }
    if (const std::string audience = ParseAudience(region); !audience.empty() && audience != "nobody") {
      return MakeTextPlan("set_mesh_capabilities", "direct_connections", audience, text, only_first);
    }
    return Ask("set_mesh_capabilities", "audience", "settings.ask.direct_audience");
  }
  return MakeBoolPlan("set_mesh_capabilities", cap.c_str(), enabled, text, only_first);
}

LocalPlan ParseMesh(const std::string& region, const std::string& text, const bool answer_mode, const std::string& carry_cap,
                    const std::string& carry_dir, const bool only_first) {
  std::vector<std::string> caps = FindCapabilities(region);
  if (caps.empty() && !carry_cap.empty()) {
    caps = {carry_cap};
  }
  Tri dir = ParseBool("set_mesh_capabilities", region, answer_mode);
  if (dir == Tri::Unclear && !carry_dir.empty()) {
    dir = carry_dir == "true" ? Tri::True : Tri::False;
  }
  if (caps.size() == 1 && dir != Tri::Unclear) {
    return MeshPlan(caps.front(), dir == Tri::True, text, only_first, region);
  }
  if (caps.size() == 1) {
    return Ask("set_mesh_capabilities", "enabled", "settings.ask.on_or_off", caps.front());
  }
  return Ask("set_mesh_capabilities", "capability", "settings.ask.mesh_capability",
             dir == Tri::Unclear ? std::string() : (dir == Tri::True ? "true" : "false"));
}

// ---------------------------------------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------------------------------------

bool WholeWordIn(const std::string& field, const std::string& name) {
  const std::string f = LowerAscii(field), n = LowerAscii(name);
  if (f.empty() || n.empty()) {
    return false;
  }
  if (f == n) {
    return true;
  }
  for (size_t p = f.find(n); p != kNpos; p = f.find(n, p + 1)) {
    const bool before_ok = p == 0 || !IsWordChar(f[p - 1]) || !IsWordChar(n.front());
    const bool after_ok = p + n.size() == f.size() || !IsWordChar(f[p + n.size()]) || !IsWordChar(n.back());
    if (before_ok && after_ok) {
      return true;
    }
  }
  return false;
}

bool PrefixOf(const std::string& field, const std::string& name) {
  const std::string f = LowerAscii(field), n = LowerAscii(name);
  return !f.empty() && !n.empty() && f.compare(0, n.size(), n) == 0;
}

std::string DisplayText(const LocalPlanContact& c) {
  return c.remark + '\x1f' + c.nickname + '\x1f' + std::to_string(c.last_contact_ms);
}

LocalPlan ResolveContact(const std::string& tool, const std::string& name, const LocalPlanContext& ctx,
                         const std::string& text, const bool only_first) {
  const std::string lowered = LowerAscii(name);
  const auto is_pronoun = std::find(kPronounWords.begin(), kPronounWords.end(), lowered) != kPronounWords.end();
  if (name.empty() || is_pronoun) {
    return Ask(tool, "contact", "contacts.ask.which_contact");
  }
  std::vector<LocalPlanContact> hits;
  for (const LocalPlanContact& c : ctx.contacts) {
    if (WholeWordIn(c.remark, name) || WholeWordIn(c.nickname, name)) {
      hits.push_back(c);
    }
  }
  if (hits.empty()) {
    for (const LocalPlanContact& c : ctx.contacts) {
      if (PrefixOf(c.remark, name) || PrefixOf(c.nickname, name)) {
        hits.push_back(c);
      }
    }
    if (hits.size() > kMaxPrefixCandidates) {
      return Ask(tool, "contact", "contacts.ask.which_contact");
    }
  }
  if (hits.empty()) {
    return LocalAmbiguous{tool, {}, false, name};
  }
  if (hits.size() == 1) {
    const LocalPlanContact& c = hits.front();
    if (tool == "open_conversation" && !c.thread_id.empty()) {
      return MakeTextPlan(tool, "thread_id", c.thread_id, text, only_first);
    }
    // A contact without a chat yet: starting one opens it.
    return MakeTextPlan("start_conversation", "contact_id", c.id, text, only_first);
  }
  LocalAmbiguous out{tool, hits, false, name};
  for (size_t i = 0; i < hits.size() && !out.identical_rows; ++i) {
    for (size_t j = i + 1; j < hits.size(); ++j) {
      if (DisplayText(hits[i]) == DisplayText(hits[j])) {
        out.identical_rows = true;
        break;
      }
    }
  }
  return out;
}

std::string CleanName(const std::string& s) {
  return StripTail(StripHead(s, kContactHeadFillerWords), kContactTailWords);
}

// ---------------------------------------------------------------------------------------------------------
// Parameter text
// ---------------------------------------------------------------------------------------------------------

/** The text between the first two spans, cut where "as / 为" starts ("Add Tom as a friend"). */
std::string GapText(const std::string& text, const std::vector<AppActionSpan>& spans) {
  std::vector<AppActionSpan> sorted = spans;
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
  if (sorted.size() < 2 || sorted[0].end >= sorted[1].begin || sorted[1].begin > text.size()) {
    return {};
  }
  std::string gap = text.substr(sorted[0].end, sorted[1].begin - sorted[0].end);
  const Haystack h(gap);
  size_t cut = kNpos;
  for (const std::string_view w : kNameConnectors) {
    if (IsAsciiPhrase(w)) {
      for (const Token& t : h.tokens) {
        if (t.word == w) {
          cut = std::min(cut, t.begin);
          break;
        }
      }
    } else if (const size_t p = h.lower.find(w); p != kNpos) {
      cut = std::min(cut, p);
    }
  }
  return cut == kNpos ? gap : gap.substr(0, cut);
}

/** What the user said in front of the hit ("我想和张三私聊"), from the clause start. */
std::string BeforeText(const std::string& text, const Parts& parts) {
  const std::string prefix = text.substr(0, parts.first_begin);
  size_t seg_begin = 0;
  for (Stop s = FindStop(prefix); s.pos != kNpos; s = FindStop(prefix.substr(seg_begin))) {
    seg_begin += s.pos + s.len;
  }
  return StripTail(StripLead(prefix.substr(seg_begin)), kActionTailFillerWords);
}

std::string CleanQuery(const std::string& s) { return StripTail(StripHead(s, kQueryFillerWords), kActionTailFillerWords); }
std::string CleanNickname(const std::string& s) {
  return TruncateCodePoints(StripTail(StripHead(s, kActionFillerWords), kActionTailFillerWords), kMaxNicknameChars);
}

bool HitsSearchVerb(const std::string& text, const std::vector<AppActionSpan>& spans) {
  for (const AppActionSpan& s : spans) {
    if (s.begin < s.end && s.end <= text.size()) {
      const std::string w = LowerAscii(text.substr(s.begin, s.end - s.begin));
      if (std::find(kSearchVerbWords.begin(), kSearchVerbWords.end(), w) != kSearchVerbWords.end()) {
        return true;
      }
    }
  }
  return false;
}

LocalPlan Dispatch(const std::string& tool, const std::string& text, const AppActionMatch* match,
                   const LocalPlanContext& ctx);

} // namespace

bool IsLocalActionQuestion(const std::string& text, const bool settings_read) { return IsQuestion(text, settings_read); }

LocalPlan AskForTool(const std::string& tool, const std::string& text) {
  switch (ClassOf(tool)) {
  case ToolClass::NoArg:
    return MakePlan(tool, Object{}, text, false);
  case ToolClass::Probe:
    return MakeBoolPlan(tool, "try_upnp", Haystack(text).Has(kUpnpWord), text, false);
  case ToolClass::Query:
    if (tool == "list_contacts") {
      return MakePlan(tool, Object{}, text, false);
    }
    return Ask(tool, "query", "contacts.ask.who");
  case ToolClass::Nickname:
    return Ask(tool, "nickname", "profile.ask.nickname");
  case ToolClass::Bool:
    return Ask(tool, "enabled", "settings.ask.on_or_off");
  case ToolClass::Appearance:
    return Ask(tool, "appearance", "settings.ask.appearance");
  case ToolClass::Language:
    return Ask(tool, "language", "settings.ask.language");
  case ToolClass::Policy:
    return Ask(tool, "policy", "settings.ask.policy");
  case ToolClass::Mesh:
    return Ask(tool, "capability", "settings.ask.mesh_capability");
  case ToolClass::Contact:
    return Ask(tool, "contact", "contacts.ask.which_contact");
  case ToolClass::Unsupported:
    break;
  }
  return LocalUnsupported{tool};
}

namespace {

LocalPlan Dispatch(const std::string& tool, const std::string& text, const AppActionMatch* match,
                   const LocalPlanContext& ctx) {
  const ToolClass cls = ClassOf(tool);
  if (cls == ToolClass::Unsupported) {
    return LocalUnsupported{tool};
  }
  const AppActionWords* words = WordsFor(tool);
  if (IsQuestion(text, words && words->settings_read)) {
    return LocalQuestion{};
  }
  if (!match || match->matched_spans.empty()) {
    return AskForTool(tool, text);
  }
  const std::vector<AppActionSpan>& spans = match->matched_spans;
  const Parts parts = SplitSentence(text, spans);
  const bool only_first = parts.more;

  switch (cls) {
  case ToolClass::NoArg:
    return MakePlan(tool, Object{}, text, only_first);
  case ToolClass::Probe:
    return MakeBoolPlan(tool, "try_upnp", Haystack(parts.region).Has(kUpnpWord), text, only_first);
  case ToolClass::Query: {
    const bool list = tool == "list_contacts";
    if (list && !HitsSearchVerb(text, spans)) {
      return MakePlan(tool, Object{}, text, only_first);
    }
    std::string q = CleanQuery(parts.rest);
    if (q.empty()) {
      q = CleanQuery(GapText(text, spans));
    }
    if (q.empty() && !list) {
      q = BeforeText(text, parts);
    }
    const std::string lowered = LowerAscii(q);
    if (std::find(kPronounWords.begin(), kPronounWords.end(), lowered) != kPronounWords.end() ||
        std::find(kGenericQueryWords.begin(), kGenericQueryWords.end(), lowered) != kGenericQueryWords.end()) {
      q.clear();
    }
    if (q.empty()) {
      return list ? MakePlan(tool, Object{}, text, only_first) : LocalPlan(Ask(tool, "query", "contacts.ask.who"));
    }
    return MakeTextPlan(tool, "query", q, text, only_first);
  }
  case ToolClass::Nickname: {
    if (parts.more || ContainsAppActionHit(parts.rest_full)) {
      return Ask(tool, "nickname", "profile.ask.nickname_confirm");
    }
    const std::string nick = CleanNickname(parts.rest);
    if (nick.empty()) {
      return Ask(tool, "nickname", "profile.ask.nickname");
    }
    return MakeTextPlan(tool, "nickname", nick, text, only_first);
  }
  case ToolClass::Bool: {
    const Tri v = ParseBool(tool, parts.region, false);
    if (v == Tri::Unclear) {
      return Ask(tool, "enabled", "settings.ask.on_or_off");
    }
    return MakeBoolPlan(tool, "enabled", v == Tri::True, text, only_first);
  }
  case ToolClass::Appearance: {
    const std::string v = ParseAppearance(parts.region, false);
    return v.empty() ? LocalPlan(Ask(tool, "appearance", "settings.ask.appearance"))
                     : MakeTextPlan(tool, "appearance", v, text, only_first);
  }
  case ToolClass::Language: {
    const std::string v = ParseLanguage(parts.region, ctx);
    return v.empty() ? LocalPlan(Ask(tool, "language", "settings.ask.language"))
                     : MakeTextPlan(tool, "language", v, text, only_first);
  }
  case ToolClass::Policy: {
    const std::string v = ParsePolicy(parts.region);
    return v.empty() ? LocalPlan(Ask(tool, "policy", "settings.ask.policy"))
                     : MakeTextPlan(tool, "policy", v, text, only_first);
  }
  case ToolClass::Mesh:
    return ParseMesh(parts.region, text, false, {}, {}, only_first);
  case ToolClass::Contact: {
    std::string name = CleanName(parts.rest);
    if (name.empty()) {
      name = CleanName(GapText(text, spans));
    }
    if (name.empty()) {
      name = CleanName(BeforeText(text, parts));
    }
    return ResolveContact(tool, name, ctx, text, only_first);
  }
  case ToolClass::Unsupported:
    break;
  }
  return LocalUnsupported{tool};
}

} // namespace

LocalPlan BuildLocalTurnPlan(const AppActionMatch& match, const std::string& text, const LocalPlanContext& ctx) {
  return Dispatch(match.tool, text, &match, ctx);
}

LocalPlan ResolvePendingAnswer(const LocalAsk& ask, const std::string& answer, const LocalPlanContext& ctx) {
  if (IsCancel(answer)) {
    return LocalCancelled{};
  }
  const std::string& tool = ask.tool;
  const std::string trimmed = StripEdges(answer);
  const AppActionWords* words = WordsFor(tool);
  const bool settings_read = words && words->settings_read;

  // Text parameters: a question is not a value; anything else is, whatever words it contains.
  if (ask.missing == "query" || ask.missing == "nickname") {
    if (IsQuestion(trimmed, settings_read)) {
      return LocalQuestion{};
    }
    const std::string value = ask.missing == "nickname" ? TruncateCodePoints(trimmed, kMaxNicknameChars) : trimmed;
    if (value.empty()) {
      return ask;
    }
    return MakeTextPlan(tool, ask.missing.c_str(), value, answer, false);
  }

  LocalPlan parsed = ask;
  if (ask.missing == "enabled" && ClassOf(tool) == ToolClass::Mesh) {
    parsed = ParseMesh(trimmed, answer, true, ask.carry, {}, false);
  } else if (ask.missing == "capability") {
    parsed = ParseMesh(trimmed, answer, true, {}, ask.carry, false);
  } else if (ask.missing == "audience") {
    if (const std::string v = ParseAudience(trimmed); !v.empty()) {
      parsed = MakeTextPlan(tool, "direct_connections", v, answer, false);
    }
  } else if (ask.missing == "enabled") {
    const Tri v = ParseBool(tool, trimmed, true);
    if (v != Tri::Unclear) {
      parsed = MakeBoolPlan(tool, "enabled", v == Tri::True, answer, false);
    }
  } else if (ask.missing == "appearance") {
    if (const std::string v = ParseAppearance(trimmed, true); !v.empty()) {
      parsed = MakeTextPlan(tool, "appearance", v, answer, false);
    }
  } else if (ask.missing == "language") {
    if (const std::string v = ParseLanguage(trimmed, ctx); !v.empty()) {
      parsed = MakeTextPlan(tool, "language", v, answer, false);
    }
  } else if (ask.missing == "policy") {
    if (const std::string v = ParsePolicy(trimmed); !v.empty()) {
      parsed = MakeTextPlan(tool, "policy", v, answer, false);
    }
  } else if (ask.missing == "contact") {
    parsed = ResolveContact(tool, CleanName(trimmed), ctx, answer, false);
  }
  if (std::holds_alternative<LocalAsk>(parsed) && std::get<LocalAsk>(parsed).missing == ask.missing &&
      std::get<LocalAsk>(parsed).carry == ask.carry && IsQuestion(trimmed, settings_read)) {
    return LocalQuestion{};
  }
  return parsed;
}

} // namespace pbr
