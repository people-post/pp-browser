#include "lib/ai/AppActionClassifier.h"
#include "lib/ai/AppActionWords.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pbr;

namespace {

std::vector<std::string> AllTools() {
  std::vector<std::string> names;
  for (const AppActionWords& words : AppActionTable()) {
    names.emplace_back(words.tool);
  }
  return names;
}

std::string Repeat(const std::string& s, const int n) {
  std::string out;
  for (int i = 0; i < n; ++i) {
    out += s;
  }
  return out;
}

// Chinese cases: ported one-to-one from brief_AI tests/test_app_actions.py (POSITIVES / NEGATIVES).
const std::vector<std::pair<std::string, std::string>> kZhPositives = {
    {"搜一下用户 Tom", "search_people"},
    {"我有哪些联系人", "list_contacts"},
    {"通讯录里有没有张三", "list_contacts"},
    {"加张三为好友", "add_contact"},
    {"把他加到联系人", "add_contact"},
    {"怎么加联系人", "add_contact"},
    {"列出我的会话", "list_conversations"},
    {"我有哪些聊天", "list_conversations"},
    {"打开和张三的聊天", "open_conversation"},
    {"给张三发消息", "start_conversation"},
    {"和张三开个聊天", "start_conversation"},
    {"帮我注册", "register_user"},
    {"把我的昵称改成小明", "update_profile_nickname"},
    {"PP 怎么改昵称", "update_profile_nickname"},
    {"我注册了吗", "get_profile_identity"},
    {"我的账号信息", "get_profile_identity"},
    {"我现在的设置是什么", "get_preferences"},
    {"我的账号安全吗", "get_security_status"},
    {"我的网络设置", "get_network_settings"},
    {"我现在能被别人连上吗", "get_reachability"},
    {"网络通不通", "get_reachability"},
    {"AI 用的是什么模型", "get_llm_settings"},
    {"接了哪些外部服务", "get_integrations"},
    {"支持哪些语言", "list_locales"},
    {"换成深色模式", "set_appearance"},
    {"把界面改成英文", "set_language"},
    {"界面改成英文", "set_language"},
    {"请帮我把界面改成英文", "set_language"},
    {"关掉通知", "set_notifications"},
    {"帮我关掉通知", "set_notifications"},
    {"关掉毛玻璃效果", "set_reduce_transparency"},
    {"不让陌生人拉我进群", "set_group_invite_policy"},
    {"打开自动续期", "set_auto_renew_registration"},
    {"打开通话诊断", "set_call_diagnostics"},
    {"打开节点模式", "set_node_enabled"},
    {"关掉 DHT", "set_mesh_capabilities"},
    {"打开中继", "set_mesh_capabilities"},
    {"重新检测一下网络", "probe_reachability"},
    {"重置 AI 的权限", "reset_tool_permissions"},
};

const std::vector<std::string> kZhNegatives = {
    "美国联系伊朗谈判了吗",
    "欧盟设置了什么关税",
    "这家公司注册在哪",
    "帮我找一下伊朗核谈判的最新新闻",
    "川普给普京发消息了吗",
    "新加坡有哪些官方语言",
    "深色模式对眼睛好吗",
    "OpenAI 最新的模型是什么",
    "检测网络攻击的方法有哪些",
    "怎么检测网络攻击",
    "中共为什么要注册所有 VPN 用户",
    "今天有什么新闻",
    "你好",
    "CNN 要告川普政府不让他们进白宫，能告赢吗？",
    "",
    "我想找人民币汇率",
    "我觉得中共的网络设置是什么意思",
    "我国为什么要重置权限",
    "让伊朗关闭中继站谈判",
    "把美国的外部服务出口限制讲一下",
    "和中共谈判的好友有哪些",
    "帮我查一下中共的节点设置",
    "苹果的新手机支持哪些语言",
    "郭文贵的通讯录里有哪些人",
    "中共如何检测网络连通来实施审查",
    "新疆学校是如何切换语言教学的",
    "中共如何要求用户注册账号实名",
    "豆包用的是什么模型",
    "伊朗网络通不通",
    "苹果怎么在中国关闭通知",
    "中共怎样重置权限",
    "给普京发消息的是谁",
    "跟普京发消息了吗",
    "如何评价中共的网络通不通",
    "怎么看中共重置权限这件事",
    "帮我把这段话润色一下：" + Repeat("联系人", 40), // more than 80 characters once normalised
};

const std::vector<std::pair<std::string, std::string>> kEnPositives = {
    {"Find someone named Tom", "search_people"},
    {"Search the directory for Tom", "search_people"},
    {"Can you look up a user called Tom?", "search_people"},
    {"Show my contacts", "list_contacts"},
    {"Please list my contacts", "list_contacts"},
    {"Add Tom as a friend", "add_contact"},
    {"How do I add a contact?", "add_contact"},
    {"List my conversations", "list_conversations"},
    {"Show my chats", "list_conversations"},
    {"Open the chat with Tom", "open_conversation"},
    {"Switch to my conversation with Tom", "open_conversation"},
    {"Send a message to Tom", "start_conversation"},
    {"Start a chat with Tom", "start_conversation"},
    {"Register this device", "register_user"},
    {"Please register me on the network", "register_user"},
    {"Change my nickname to Sam", "update_profile_nickname"},
    {"How do I change my username?", "update_profile_nickname"},
    {"Am I registered?", "get_profile_identity"},
    {"What is my peerid?", "get_profile_identity"},
    {"Show my settings", "get_preferences"},
    {"What are my current settings?", "get_preferences"},
    {"Is my account secure?", "get_security_status"},
    {"Check my security status", "get_security_status"},
    {"Show my network settings", "get_network_settings"},
    {"Can you show the relay settings?", "get_network_settings"},
    {"Am I reachable from outside?", "get_reachability"},
    {"Can others reach my device?", "get_reachability"},
    {"What model are you using?", "get_llm_settings"},
    {"Show my AI settings", "get_llm_settings"},
    {"Which external services are connected?", "get_integrations"},
    {"List my integrations", "get_integrations"},
    {"Which languages are supported?", "list_locales"},
    {"What languages are available?", "list_locales"},
    {"Switch to dark mode", "set_appearance"},
    {"Turn on dark mode please", "set_appearance"},
    {"follow the system", "set_appearance"},
    {"match the system", "set_appearance"},
    {"Change the language to Chinese", "set_language"},
    {"Switch the interface to English", "set_language"},
    {"Turn off notifications", "set_notifications"},
    {"Please disable notifications", "set_notifications"},
    {"Reduce transparency", "set_reduce_transparency"},
    {"Turn off the frosted glass effect", "set_reduce_transparency"},
    {"Block group invites from strangers", "set_group_invite_policy"},
    {"Don't let strangers add me to groups", "set_group_invite_policy"},
    {"Turn on auto-renew", "set_auto_renew_registration"},
    {"Please enable auto renewal", "set_auto_renew_registration"},
    {"Turn on call diagnostics", "set_call_diagnostics"},
    {"Show call diagnostics", "set_call_diagnostics"},
    {"Enable node mode", "set_node_enabled"},
    {"Turn off node mode", "set_node_enabled"},
    {"Turn off DHT", "set_mesh_capabilities"},
    {"Enable the relay", "set_mesh_capabilities"},
    {"Test my network", "probe_reachability"},
    {"Check network connectivity again", "probe_reachability"},
    {"Reset AI permissions", "reset_tool_permissions"},
    {"Reset the tool permissions", "reset_tool_permissions"},
};

const std::vector<std::string> kEnNegatives = {
    "Did the US contact Iran about talks?",
    // "my" / "I" that do not qualify the tool's own words: someone's story, not a request to the app.
    "Should I switch to English for my thesis?",
    "My friend said Apple will turn off notifications next year",
    "Why did my teacher say I should enable dark mode?",
    "Is it true that WhatsApp lets you mute notifications? I wonder",
    "I saw that Apple will turn off notifications for news apps",
    "My company wants to register an account in Delaware",
    "Can I mute notifications on WhatsApp?",
    // A person word followed by a clause is a question about the world, not a directory search.
    "Can you find people who survived the Titanic?",
    "Help me find someone to fix my car",
    "Please search for users affected by the breach",
    "What tariffs did the EU set?",
    "Where is this company registered?",
    "What is in the news today?",
    "Find the latest news about Musk",
    "Search for the latest news on the Iran talks",
    "Did Trump send a message to Putin?",
    "Is dark mode good for your eyes?",
    "What model does OpenAI use?",
    "Which languages are supported by the iPhone camera?",
    "How do you think the EU should set tariffs?",
    "I think the new settings of the Fed are a mistake",
    "I heard China wants every VPN user to register",
    "Why does China require VPN users to register?",
    "How to detect network attacks?",
    "What are the official languages of Singapore?",
    "Hello",
    "Who is the contact person at the embassy?",
    "Did the EU change its language policy?",
    "Show the latest chats leaked from the embassy",
    "",
};

std::string Describe(const std::string& message) { return "message: " + message.substr(0, 60); }

} // namespace

TEST(AppActionClassifierTest, EveryToolHasAChineseAndAnEnglishPositive) {
  ASSERT_EQ(AllTools().size(), 27u);
  for (const auto& cases : {kZhPositives, kEnPositives}) {
    std::vector<std::string> covered;
    for (const auto& c : cases) {
      covered.push_back(c.second);
    }
    for (const std::string& tool : AllTools()) {
      EXPECT_NE(std::find(covered.begin(), covered.end(), tool), covered.end()) << tool;
    }
  }
}

TEST(AppActionClassifierTest, ChinesePositives) {
  for (const auto& [message, expected] : kZhPositives) {
    EXPECT_EQ(ClassifyAppAction(message, AllTools()), expected) << Describe(message);
  }
}

TEST(AppActionClassifierTest, ChineseNegatives) {
  for (const std::string& message : kZhNegatives) {
    EXPECT_EQ(ClassifyAppAction(message, AllTools()), std::nullopt) << Describe(message);
  }
}

TEST(AppActionClassifierTest, EnglishPositives) {
  for (const auto& [message, expected] : kEnPositives) {
    EXPECT_EQ(ClassifyAppAction(message, AllTools()), expected) << Describe(message);
  }
}

TEST(AppActionClassifierTest, EnglishNegatives) {
  for (const std::string& message : kEnNegatives) {
    EXPECT_EQ(ClassifyAppAction(message, AllTools()), std::nullopt) << Describe(message);
  }
}

TEST(AppActionClassifierTest, ABareEnglishFindNameAsksTheDirectory) {
  for (const char* message : {"find Ken", "Find Ken Lee", "look for Ken", "please find Ken", "can you find Ken"}) {
    EXPECT_EQ(ClassifyAppAction(message, AllTools()), std::optional<std::string>("search_people")) << Describe(message);
  }
  for (const char* message : {"find ken"}) {
    EXPECT_EQ(ClassifyAppAction(message, AllTools()), std::optional<std::string>("search_people")) << Describe(message);
  }
  for (const char* message : {"find cheap flights", "find pizza places", "find Tokyo weather", "search for restaurants",
                              "find news about Iran", "find out", "find something to eat", "find the best pizza place",
                              "Find people who survived the Titanic"}) {
    EXPECT_NE(ClassifyAppAction(message, AllTools()), std::optional<std::string>("search_people")) << Describe(message);
  }
  // Without the tool declared the rule is off.
  EXPECT_EQ(ClassifyAppAction("find Ken", {"get_preferences"}), std::nullopt);
}

TEST(AppActionClassifierTest, OverviewQuestions) {
  for (const char* q : {"你能做什么？", "你会做什么", "你能干什么?", "你会什么", "你能帮我做什么", "你有什么功能。", "能做什么", " 你能做什么 ",
                        "What can you do?", "what do you do", "What can you help with?", "What are you able to do?",
                        "what can you help me with", "你能做什么呢", "那你都能做什么呀？", "请问你会干什么吗", "你好，你能帮我干什么",
                        "你可以做什么啊", "你有哪些功能", "会做什么", "Hi, what can you do for me?", "hey what else can you do",
                        "so what all can you do", "What things can you do?", "hello what do you do"}) {
    EXPECT_TRUE(IsAppOverviewQuestion(q)) << q;
  }
  for (const char* q : {"what can you do about the weather", "你能做什么菜", "今天有什么新闻", "", "can you do it", "你能做什么菜呢", "你好吗"}) {
    EXPECT_FALSE(IsAppOverviewQuestion(q)) << q;
  }
}

TEST(AppActionClassifierTest, EnglishPositivesAreDeclaredToolsOnly) {
  for (const auto& [message, expected] : kEnPositives) {
    EXPECT_EQ(ClassifyAppAction(message, {expected}), expected) << Describe(message);
    EXPECT_EQ(ClassifyAppAction(message, {"no_such_tool"}), std::nullopt) << Describe(message);
  }
}

TEST(AppActionClassifierTest, OnlyDeclaredToolsCount) {
  EXPECT_EQ(ClassifyAppAction("加张三为好友", {"set_language"}), std::nullopt);
  EXPECT_EQ(ClassifyAppAction("加张三为好友", {}), std::nullopt);
  EXPECT_EQ(ClassifyAppAction("加张三为好友", {"unknown_cap", "add_contact"}), "add_contact");
  EXPECT_EQ(ClassifyAppAction("Add Tom as a friend", {"unknown_cap", "add_contact"}), "add_contact");
}

TEST(AppActionClassifierTest, AddFriendRequestsReachSearchWhenAdditionRequiresSelectingAResult) {
  const std::vector<std::string> tools = {"search_people", "list_contacts"};
  for (const std::string message : {
           "帮我加小明为好友", "帮我添加昵称小明为好友", "加 account:abc_DEF-123 为好友",
           "Add Tom as a friend", "Please add relay:abc123 as a contact"}) {
    EXPECT_EQ(ClassifyAppAction(message, tools), "search_people") << message;
  }
  EXPECT_EQ(ClassifyAppAction("帮我搜索昵称小明", tools), "search_people");
  EXPECT_EQ(ClassifyAppAction("通讯录里有没有张三", tools), "list_contacts");
  EXPECT_EQ(ClassifyAppAction("加张三为好友", {"list_contacts"}), std::nullopt);
}

TEST(AppActionClassifierTest, TooLongMessagesAreNotJudged) {
  EXPECT_EQ(ClassifyAppAction(Repeat("联系人", 40), AllTools()), std::nullopt);
  EXPECT_EQ(ClassifyAppAction("Show my contacts " + Repeat("and more ", 30), AllTools()), std::nullopt);
}

TEST(AppActionClassifierTest, ChineseLengthIsCountedInCharactersNotBytes) {
  // 24 characters (72 bytes) of padding after the request stay under the 80-character limit.
  EXPECT_EQ(ClassifyAppAction("我有哪些联系人" + Repeat("啊", 24), AllTools()), "list_contacts");
  EXPECT_EQ(ClassifyAppAction("我有哪些联系人" + Repeat("啊", 80), AllTools()), std::nullopt);
}

TEST(AppActionClassifierTest, EnglishWhitespaceAndCaseAreNormalised) {
  EXPECT_EQ(ClassifyAppAction("  SHOW   my	CONTACTS  ", AllTools()), "list_contacts");
  EXPECT_EQ(ClassifyAppAction("Please, turn off notifications!", AllTools()), "set_notifications");
}

TEST(AppActionClassifierTest, PhraseHitsBeatVerbObjectHitsInDeclaredOrder) {
  // Both tools hit "open the chat with Tom": open_conversation by phrase, list_conversations by neither.
  EXPECT_EQ(ClassifyAppAction("Open the chat with Tom", {"list_conversations", "open_conversation"}), "open_conversation");
}

namespace {

std::vector<std::string> SpanTexts(const std::string& text, const AppActionMatch& match) {
  std::vector<std::string> out;
  for (const AppActionSpan& s : match.matched_spans) {
    out.push_back(text.substr(s.begin, s.end - s.begin));
  }
  return out;
}

} // namespace

TEST(AppActionClassifierTest, MatchReturnsTheToolAndWhereTheWordsHit) {
  // A phrase hit is one span.
  const std::string phrase = "搜索用户 ken";
  const auto a = ClassifyAppActionMatch(phrase, AllTools());
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->tool, "search_people");
  EXPECT_EQ(SpanTexts(phrase, *a), (std::vector<std::string>{"搜索用户"}));

  // The matcher drops whitespace; the span still maps back onto the original bytes, space included.
  const std::string verb_object = "关掉 DHT";
  const auto b = ClassifyAppActionMatch(verb_object, AllTools());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->tool, "set_mesh_capabilities");
  EXPECT_EQ(SpanTexts(verb_object, *b), (std::vector<std::string>{"关掉 DHT"}));

  // A verb + object hit is two spans, in order.
  const std::string two = "帮我关掉通知";
  const auto b2 = ClassifyAppActionMatch(two, AllTools());
  ASSERT_TRUE(b2.has_value());
  ASSERT_EQ(b2->matched_spans.size(), 1u) << "phrase 关掉通知";
  EXPECT_EQ(SpanTexts(two, *b2), (std::vector<std::string>{"关掉通知"}));

  const std::string en = "Find user bob";
  const auto c = ClassifyAppActionMatch(en, AllTools());
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(SpanTexts(en, *c), (std::vector<std::string>{"Find", "user"}));

  const std::string en_phrase = "Please turn off notifications now";
  const auto d = ClassifyAppActionMatch(en_phrase, AllTools());
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(SpanTexts(en_phrase, *d), (std::vector<std::string>{"turn off notifications"}));

  // A curly apostrophe is part of the word and does not shift the offsets after it.
  const std::string curly = "Don\xE2\x80\x99t panic. Show my contacts";
  const auto e = ClassifyAppActionMatch(curly, AllTools());
  ASSERT_TRUE(e.has_value());
  ASSERT_EQ(e->matched_spans.size(), 1u);
  EXPECT_EQ(curly.substr(e->matched_spans[0].begin, e->matched_spans[0].end - e->matched_spans[0].begin), "Show my contacts");
}

TEST(AppActionClassifierTest, AddContactMappedToSearchKeepsTheAddWordsSpans) {
  std::vector<std::string> tools = AllTools();
  tools.erase(std::remove(tools.begin(), tools.end(), "add_contact"), tools.end());
  const std::string text = "加张三为好友";
  const auto m = ClassifyAppActionMatch(text, tools);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->tool, "search_people");
  EXPECT_EQ(SpanTexts(text, *m), (std::vector<std::string>{"为好友"}));
}

TEST(AppActionClassifierTest, MatchAgreesWithClassifyAppActionAndStaysInsideTheText) {
  const auto check = [](const std::string& text) {
    const auto tool = ClassifyAppAction(text, AllTools());
    const auto match = ClassifyAppActionMatch(text, AllTools());
    ASSERT_EQ(tool.has_value(), match.has_value()) << text;
    if (!match) {
      return;
    }
    EXPECT_EQ(*tool, match->tool) << text;
    EXPECT_FALSE(match->matched_spans.empty()) << text;
    for (const AppActionSpan& s : match->matched_spans) {
      EXPECT_LT(s.begin, s.end) << text;
      EXPECT_LE(s.end, text.size()) << text;
    }
  };
  for (const auto& [text, tool] : kZhPositives) {
    check(text);
  }
  for (const auto& [text, tool] : kEnPositives) {
    check(text);
  }
  for (const std::string& text : kZhNegatives) {
    check(text);
  }
}

TEST(AppActionClassifierTest, ContainsAppActionHitIgnoresTheRequestForm) {
  EXPECT_TRUE(ContainsAppActionHit("Bob 打开通知"));
  EXPECT_TRUE(ContainsAppActionHit("Bob turn off notifications"));
  EXPECT_FALSE(ContainsAppActionHit("Tony the Great"));
  EXPECT_FALSE(ContainsAppActionHit("小成"));
  EXPECT_FALSE(ContainsAppActionHit(""));
}

// Device check 2026-10-28: the everyday phrasings that went to brief_AI instead of the device.
TEST(AppActionClassifierTest, EverydayPhrasings) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"搜索用户 ken", "search_people"},
      {"找一下 Alice", "search_people"},
      {"搜一下 Alice", "search_people"},
      {"look up Ken", "search_people"},
      {"看一下我的设置", "get_preferences"},
      {"我的设置", "get_preferences"},
      {"帮我看一下设置", "get_preferences"},
      {"看一下设置", "get_preferences"},
      {"my settings", "get_preferences"},
      {"我的网络状态", "get_reachability"},
      {"看看网络状态", "get_reachability"},
      {"网络状态", "get_reachability"},
      {"network status", "get_reachability"},
      {"我的安全状态", "get_security_status"},
      {"深色模式", "set_appearance"},
      {"浅色模式", "set_appearance"},
      {"夜间模式", "set_appearance"},
      {"跟随系统", "set_appearance"},
      {"切换到深色模式", "set_appearance"},
      {"换到浅色模式", "set_appearance"},
      {"改到夜间模式", "set_appearance"},
      {"dark mode", "set_appearance"},
      {"切换到中文", "set_language"},
      {"switch to English", "set_language"},
  };
  for (const auto& [text, tool] : cases) {
    const auto match = ClassifyAppActionMatch(text, AllTools());
    ASSERT_TRUE(match.has_value()) << text;
    EXPECT_EQ(match->tool, tool) << text;
  }
  for (const char* text : {"今天天气怎么样", "帮我写首诗", "我的朋友很多", "设置一个闹钟", "找", "find", "look up the capital of France",
                           "dark mode is overrated in my opinion"}) {
    EXPECT_FALSE(ClassifyAppActionMatch(text, AllTools()).has_value()) << text;
  }
}

// Simulator run 2026-10-28: a bare setting name is a hit (the planner asks on or off); a sentence that merely
// contains the word is not.
TEST(AppActionClassifierTest, BareBooleanSettingNames) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"通知呢", "set_notifications"},
      {"通知", "set_notifications"},
      {"通知？", "set_notifications"},
      {"Notifications?", "set_notifications"},
      {"notifications", "set_notifications"},
      {"节点呢", "set_node_enabled"},
      {"透明度呢", "set_reduce_transparency"},
      {"毛玻璃", "set_reduce_transparency"},
      {"自动续期呢", "set_auto_renew_registration"},
      {"通话诊断吗", "set_call_diagnostics"},
      {"call diagnostics?", "set_call_diagnostics"},
      {"node mode", "set_node_enabled"},
      {"blur?", "set_reduce_transparency"},
  };
  for (const auto& [text, tool] : cases) {
    const auto match = ClassifyAppActionMatch(text, AllTools());
    ASSERT_TRUE(match.has_value()) << text;
    EXPECT_EQ(match->tool, tool) << text;
    ASSERT_FALSE(match->matched_spans.empty()) << text;
    EXPECT_LT(match->matched_spans[0].begin, match->matched_spans[0].end) << text;
    EXPECT_LE(match->matched_spans[0].end, text.size()) << text;
  }
  for (const char* text : {"通知我一下", "通知他明天开会", "notification sounds are annoying", "blur the background", "节点在哪"}) {
    EXPECT_FALSE(ClassifyAppActionMatch(text, AllTools()).has_value()) << text;
  }
}
