#pragma once

#include "common/ContactLimits.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace pbr {

/**
 * Word tables of the on-device parameter extraction (LocalActionPlanner.h). Chinese words are matched as
 * substrings of the lowercased sentence, English ones as whole words (and word sequences). Everything is
 * lowercase; tests and the render step refer to these by name.
 */

inline constexpr size_t kMaxPrefixCandidates = 5;      // more prefix hits than this: ask for a longer name
inline constexpr size_t kMaxCancelChars = 12;          // a cancel word counts only in a short answer
inline constexpr size_t kNegationWindowChars = 4;      // Chinese: negation this close before a direction word
inline constexpr size_t kNegationWindowWords = 3;      // English: same, in words

/** TurnPlan::synthesis_hints of a plan whose sentence had a second action that was not taken. */
inline constexpr std::string_view kHintOnlyFirstAction = "only_first_action";

// --- Sentence shape ---------------------------------------------------------------------------------------

/** "How / why" questions. A question is handed to the streamed answer; polite requests are not questions. */
inline constexpr auto kQuestionWordsZh = std::to_array<std::string_view>(
    {"怎么", "怎样", "如何", "咋", "为什么", "为啥", "什么是", "啥是", "什么意思", "有什么用"});
inline constexpr auto kQuestionWordsEn = std::to_array<std::string_view>(
    {"how to", "how do i", "how can i", "how should i", "how does", "how do you", "why"});
/** "X is what" — a question for write tools, but "我的设置是什么" is a read request, so settings-read tools skip these. */
inline constexpr auto kWhatIsWordsZh = std::to_array<std::string_view>({"是什么", "是啥"});
inline constexpr auto kWhatIsWordsEn = std::to_array<std::string_view>({"what is", "what's", "what are", "what does"});

/** Where the parameter text ends: the next action starts here. */
inline constexpr auto kStopWordsZh = std::to_array<std::string_view>(
    {"然后", "并且", "顺便", "还有", "再", "并", "，", "。", "；"});
inline constexpr auto kStopWordsEn = std::to_array<std::string_view>({"and", "then", "also"});
inline constexpr auto kStopChars = std::to_array<std::string_view>({",", ";"});

/** Stripped only from the head of the parameter text (adjacent to the hit), never from the middle. */
inline constexpr auto kActionFillerWords = std::to_array<std::string_view>(
    {"一下", "帮我", "请", "把", "为", "叫", "成", "的", "是", "名叫", "名为", "the", "user", "users", "a", "an",
     "my", "to", "named", "called", "with"});
/** The head fillers that are safe in front of a search query ("成龙", "为什么" must survive). */
inline constexpr auto kQueryFillerWords = std::to_array<std::string_view>(
    {"一下", "名叫", "名为", "叫", "是", "the", "user", "users", "a", "an", "my", "named", "called", "for", "with"});
/** Head fillers of a contact name; the hit often ends before "和 / 跟 / with". */
inline constexpr auto kContactHeadFillerWords = std::to_array<std::string_view>(
    {"一下", "帮我", "请", "和", "跟", "与", "给", "the", "my", "with", "to"});
/** Stripped from the tail of the parameter text. */
inline constexpr auto kActionTailFillerWords = std::to_array<std::string_view>(
    {"一下", "吧", "啊", "呀", "嘛", "谢谢", "please", "thanks", "thank you"});
/** Stripped from the tail of a contact name ("打开和 Tom 的聊天"). */
inline constexpr auto kContactTailWords = std::to_array<std::string_view>(
    {"的聊天", "的会话", "的对话", "的消息", "聊天", "会话", "对话", "消息", "chat", "conversation", "thread"});
/** Words in front of a name that the user said before it ("我想和张三私聊", "Add Tom"). */
inline constexpr auto kLeadWords = std::to_array<std::string_view>(
    {"请", "帮我", "帮忙", "给我", "替我", "麻烦", "把", "让", "我想要", "我要", "我想", "能不能", "能否", "可不可以",
     "可以", "加上", "添加", "加", "找", "搜索", "搜", "查找", "给", "和", "跟", "与", "please", "can you", "could you",
     "would you", "help me", "i want to", "i'd like to", "add", "find", "search", "look up"});
/** "Add Tom as a friend": the name ends where one of these starts (only used between the verb and the object). */
inline constexpr auto kNameConnectors = std::to_array<std::string_view>({"为", "当", "作为", "成", "as", "to", "into"});
/** A pronoun is not a name to look up. */
inline constexpr auto kPronounWords = std::to_array<std::string_view>(
    {"他", "她", "它", "他们", "她们", "这个人", "那个人", "此人", "he", "him", "she", "her", "them", "they",
     "this person", "that person"});
/** "帮我找人": naming no one is not a query. */
inline constexpr auto kGenericQueryWords = std::to_array<std::string_view>(
    {"人", "人员", "某人", "个人", "用户", "someone", "somebody", "people", "person", "user", "users"});
/** Search verbs of list_contacts: only then is the text after the hit a query. */
inline constexpr auto kSearchVerbWords = std::to_array<std::string_view>(
    {"找", "搜", "搜索", "查找", "通讯录里有没有", "find", "search", "look up", "look for"});

// --- On / off -----------------------------------------------------------------------------------------------

inline constexpr auto kBoolOnWordsZh = std::to_array<std::string_view>(
    {"打开", "开启", "启用", "开通", "启动", "显示", "恢复", "当节点", "作为节点", "成为节点"});
inline constexpr auto kBoolOffWordsZh = std::to_array<std::string_view>({"关闭", "关掉", "停用", "禁用", "静音", "隐藏"});
inline constexpr auto kBoolOnWordsEn = std::to_array<std::string_view>(
    {"on", "enable", "enabled", "activate", "unmute", "show", "become", "run as", "act as"});
inline constexpr auto kBoolOffWordsEn = std::to_array<std::string_view>({"off", "disable", "disabled", "mute", "hide", "stop"});
/** One character only counts right next to the object word or at the end of the sentence. */
inline constexpr std::string_view kBoolSingleOn = "开";
inline constexpr std::string_view kBoolSingleOff = "关";
/** Words that contain 开 / 关 without meaning on / off ("打开通知开关", "关于"). */
inline constexpr auto kBoolMaskedWords = std::to_array<std::string_view>(
    {"开关", "关于", "关系", "关注", "关键", "开始", "开发", "开头", "开放"});
/** Sentence-final particles that do not stop 开 / 关 from being "at the end". */
inline constexpr auto kTrailingParticles = std::to_array<std::string_view>(
    {"了", "吧", "啊", "呀", "呢", "吗", "哦", "嘛", "一下", "!", "！", "。", ".", "?", "？", "~", " "});
inline constexpr auto kNegationWordsZh = std::to_array<std::string_view>({"不", "别", "勿", "没", "无需"});
inline constexpr auto kNegationWordsEn = std::to_array<std::string_view>(
    {"not", "never", "don't", "dont", "no", "cannot", "can't", "without"});
/** "能不能关掉" is a polite request: the 不 in these is not a negation. */
inline constexpr auto kNegationMaskedWords = std::to_array<std::string_view>(
    {"能不能", "可不可以", "是不是", "要不要", "好不好", "行不行", "对不对", "有没有"});
/** set_reduce_transparency: the setting is "reduce"; "turn off the blur" therefore means enabled = true. */
inline constexpr auto kReduceWords = std::to_array<std::string_view>({"减少", "reduce"});

// --- Enumerations -------------------------------------------------------------------------------------------

inline constexpr auto kAppearanceDarkWords = std::to_array<std::string_view>({"深色", "暗色", "夜间", "夜晚", "dark", "night"});
inline constexpr auto kAppearanceLightWords = std::to_array<std::string_view>({"浅色", "亮色", "白天", "light", "bright"});
inline constexpr auto kAppearanceSystemWords = std::to_array<std::string_view>({"跟随系统", "系统", "自动", "system", "auto"});

/** Values of set_group_invite_policy / set_call_invite_policy: everyone, contacts_only, nobody. */
inline constexpr auto kPolicyAnyoneWords = std::to_array<std::string_view>(
    {"任何人", "所有人", "谁都", "anyone", "everyone", "anybody"});
inline constexpr auto kPolicyContactsWords = std::to_array<std::string_view>(
    {"联系人", "好友", "熟人", "朋友", "contacts", "contact", "friends", "friend"});
inline constexpr auto kPolicyNobodyWords = std::to_array<std::string_view>(
    {"没人", "没有人", "谁都不", "nobody", "no one"});
/** "不允许 + anyone" means nobody; "不让 + strangers" means contacts only. */
inline constexpr auto kPolicyBlockWords = std::to_array<std::string_view>(
    {"不允许", "不让", "禁止", "不许", "不准", "别让", "block", "forbid", "don't allow", "do not allow", "don't let",
     "do not let"});
inline constexpr auto kPolicyStrangerWords = std::to_array<std::string_view>({"陌生人", "stranger", "strangers"});

inline constexpr auto kLocaleChineseWords = std::to_array<std::string_view>({"中文", "简体", "汉语", "chinese", "zh"});
inline constexpr auto kLocaleTraditionalWords = std::to_array<std::string_view>({"繁体", "繁體", "traditional"});
inline constexpr auto kLocaleEnglishWords = std::to_array<std::string_view>({"英文", "英语", "english", "en"});

/** set_mesh_capabilities: one capability per request. */
inline constexpr auto kCapMediaRelayWords = std::to_array<std::string_view>({"媒体中继", "media relay"});
inline constexpr auto kCapRelayWords = std::to_array<std::string_view>({"中继", "relay"});
inline constexpr auto kCapDhtWords = std::to_array<std::string_view>({"dht"});
inline constexpr auto kCapPreferContactsWords = std::to_array<std::string_view>({"优先联系人", "prefer contacts"});
inline constexpr auto kCapDirectWords = std::to_array<std::string_view>({"直连", "direct"});

inline constexpr std::string_view kUpnpWord = "upnp";

// --- Follow-up answers --------------------------------------------------------------------------------------

inline constexpr auto kCancelWords = std::to_array<std::string_view>(
    {"算了", "取消", "不用了", "不改了", "别了", "never mind", "nevermind", "cancel", "forget it"});

} // namespace pbr
