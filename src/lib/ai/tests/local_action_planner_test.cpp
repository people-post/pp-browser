#include "lib/ai/AppActionClassifier.h"
#include "lib/ai/AppActionWords.h"
#include "lib/ai/LocalActionPlanner.h"
#include "lib/ai/LocalActionWords.h"

#include <gtest/gtest.h>

#include <optional>
#include <algorithm>
#include <string>
#include <vector>

using namespace pbr;

namespace {

std::vector<std::string> AllTools() {
  std::vector<std::string> names;
  for (const AppActionWords& words : AppActionTable()) {
    names.emplace_back(words.tool);
  }
  // The client declares no add_contact tool: add requests reach search_people. set_call_invite_policy has no
  // wordlist entry.
  names.erase(std::remove(names.begin(), names.end(), "add_contact"), names.end());
  names.emplace_back("set_call_invite_policy");
  return names;
}

/** The sentence goes through the real classifier, then the planner. */
LocalPlan PlanFor(const std::string& text, const LocalPlanContext& ctx = {}) {
  const auto match = ClassifyAppActionMatch(text, AllTools());
  EXPECT_TRUE(match.has_value()) << "not classified: " << text;
  if (!match) {
    return LocalQuestion{};
  }
  return BuildLocalTurnPlan(*match, text, ctx);
}

/** For sentences the wordlist does not classify (no object word, tool outside the table): the hit is `hit`. */
LocalPlan PlanAs(const std::string& tool, const std::string& text, const std::string& hit, const LocalPlanContext& ctx = {}) {
  AppActionMatch match{tool, {}};
  if (!hit.empty()) {
    const size_t at = text.find(hit);
    EXPECT_NE(at, std::string::npos) << hit << " not in " << text;
    match.matched_spans.push_back({at, at + hit.size()});
  }
  return BuildLocalTurnPlan(match, text, ctx);
}

const TurnPlan* AsPlan(const LocalPlan& p) { return std::get_if<TurnPlan>(&p); }

std::string Hints(const LocalPlan& p) {
  const TurnPlan* plan = AsPlan(p);
  return plan ? plan->synthesis_hints : "<not a plan>";
}

std::optional<std::string> Arg(const LocalPlan& p, const char* key) {
  const TurnPlan* plan = AsPlan(p);
  return plan && !plan->tools.empty() ? plan->tools[0].arguments.getString(key) : std::nullopt;
}

void ExpectPlan(const LocalPlan& p, const std::string& tool, const std::string& context) {
  const TurnPlan* plan = AsPlan(p);
  ASSERT_NE(plan, nullptr) << context;
  ASSERT_EQ(plan->tools.size(), 1u) << context;
  EXPECT_EQ(plan->tools[0].name, tool) << context;
  EXPECT_TRUE(static_cast<bool>(ValidateTurnPlan(*plan, AllTools()))) << context;
}

void ExpectText(const LocalPlan& p, const std::string& tool, const char* key, const std::string& value, const std::string& context) {
  ExpectPlan(p, tool, context);
  if (const TurnPlan* plan = AsPlan(p)) {
    EXPECT_EQ(plan->tools[0].arguments.getString(key), value) << context;
  }
}

void ExpectBool(const LocalPlan& p, const std::string& tool, const char* key, const bool value, const std::string& context) {
  ExpectPlan(p, tool, context);
  if (const TurnPlan* plan = AsPlan(p)) {
    EXPECT_EQ(plan->tools[0].arguments.getIf<bool>(key), std::optional<bool>(value)) << context;
  }
}

void ExpectAsk(const LocalPlan& p, const std::string& key, const std::string& context) {
  const auto* ask = std::get_if<LocalAsk>(&p);
  ASSERT_NE(ask, nullptr) << context;
  EXPECT_EQ(ask->question_key, key) << context;
}

LocalPlanContact Contact(const std::string& id, const std::string& remark, const std::string& nickname,
                         const int64_t last = 0, const std::string& thread = "") {
  return LocalPlanContact{id, thread.empty() ? "t-" + id : thread, remark, nickname, last};
}

} // namespace

// --- parameter text ---------------------------------------------------------------------------------------

TEST(LocalActionPlannerTest, SearchTakesTheTextAfterTheHit) {
  ExpectText(PlanFor("搜索用户 ken"), "search_people", "query", "ken", "搜索用户 ken");
  ExpectText(PlanAs("search_people", "找一下 Alice", "找"), "search_people", "query", "Alice", "找一下 Alice");
  ExpectText(PlanFor("find user bob"), "search_people", "query", "bob", "find user bob");
  ExpectText(PlanAs("search_people", "look up Ken", "look up"), "search_people", "query", "Ken", "look up Ken");
  ExpectText(PlanFor("搜索用户 王叫兽"), "search_people", "query", "王叫兽", "王叫兽");
  ExpectText(PlanFor("搜索用户 成龙"), "search_people", "query", "成龙", "成龙 keeps its first character");
  ExpectText(PlanFor("Search the directory for Tom"), "search_people", "query", "Tom", "directory for Tom");
  ExpectText(PlanFor("Can you look up a user called Tom?"), "search_people", "query", "Tom", "user called Tom");
}

TEST(LocalActionPlannerTest, ParameterEndsAtTheStopWordAndTheRestIsNotTaken) {
  const LocalPlan p = PlanFor("搜索用户 ken，然后打开通知");
  ExpectText(p, "search_people", "query", "ken", "stop word");
  EXPECT_EQ(Hints(p), kHintOnlyFirstAction);
  const LocalPlan q = PlanAs("search_people", "find user bob and then turn off notifications", "find");
  ExpectText(q, "search_people", "query", "bob", "english stop word");
  EXPECT_EQ(Hints(q), kHintOnlyFirstAction);
  // A trailing mark with nothing behind it is not a second action.
  EXPECT_EQ(Hints(PlanFor("搜索用户 ken。")), "");
  EXPECT_EQ(Hints(PlanFor("搜索用户 ken")), "");
}

// 再 / 并 are not stop words: they occur inside names.
TEST(LocalActionPlannerTest, SingleCharacterConnectivesStayInTheName) {
  ExpectText(PlanFor("搜索用户 再见青春"), "search_people", "query", "再见青春", "再见青春");
  ExpectText(PlanFor("改昵称为 李并"), "update_profile_nickname", "nickname", "李并", "李并");
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "", "王再明", 0, "thread-1")};
  ExpectText(PlanFor("打开和 王再明 的聊天", ctx), "open_conversation", "thread_id", "thread-1", "王再明");
}

TEST(LocalActionPlannerTest, NamesAreTakenAsTheyAre) {
  ExpectText(PlanFor("改昵称为 Bob"), "update_profile_nickname", "nickname", "Bob", "改昵称为 Bob");
  ExpectText(PlanFor("change my nickname to Bob"), "update_profile_nickname", "nickname", "Bob", "change to Bob");
  ExpectText(PlanFor("改昵称为 小成"), "update_profile_nickname", "nickname", "小成", "小成 keeps its last character");
  ExpectText(PlanFor("改昵称为成龙"), "update_profile_nickname", "nickname", "成龙", "成龙 keeps its first character");
  ExpectText(PlanFor("change my nickname to Tony the Great"), "update_profile_nickname", "nickname", "Tony the Great", "Tony");
  ExpectText(PlanFor("把昵称改成 Bob。"), "update_profile_nickname", "nickname", "Bob", "trailing mark");
}

TEST(LocalActionPlannerTest, NicknameWithASecondActionIsAskedBack) {
  ExpectAsk(PlanFor("把昵称改成 Bob 然后打开通知"), "profile.ask.nickname_confirm", "stop word");
  ExpectAsk(PlanFor("改昵称为 Bob 打开通知"), "profile.ask.nickname_confirm", "second hit without a stop word");
  ExpectAsk(PlanFor("change my nickname to Tom and Jerry"), "profile.ask.nickname_confirm", "english and");
  ExpectAsk(PlanFor("改昵称"), "profile.ask.nickname", "empty");
  ExpectAsk(PlanFor("帮我注册"), "profile.ask.nickname", "register");
  EXPECT_EQ(std::get<LocalAsk>(PlanFor("改昵称")).missing, "nickname");
}

TEST(LocalActionPlannerTest, NicknameIsCutToTheLimit) {
  const std::string long_name(60, 'a');
  const LocalPlan p = PlanFor("改昵称为 " + long_name);
  ASSERT_TRUE(Arg(p, "nickname").has_value());
  EXPECT_EQ(Arg(p, "nickname")->size(), kMaxNicknameChars);
}

TEST(LocalActionPlannerTest, SearchWithoutAnyoneNamedIsAsked) {
  ExpectAsk(PlanAs("search_people", "帮我找人", "找"), "contacts.ask.who", "帮我找人");
  ExpectAsk(PlanFor("find someone"), "contacts.ask.who", "find someone");
  ExpectAsk(PlanFor("把他加到联系人"), "contacts.ask.who", "pronoun");
  ExpectText(PlanFor("加张三为好友"), "search_people", "query", "张三", "add by name");
  ExpectText(PlanFor("Add Tom as a friend"), "search_people", "query", "Tom", "add by name (english)");
}

TEST(LocalActionPlannerTest, ListContactsTakesAQueryOnlyAfterASearchVerb) {
  ExpectPlan(PlanFor("我有哪些联系人"), "list_contacts", "list all");
  ExpectPlan(PlanFor("show my contacts"), "list_contacts", "list all (en)");
  EXPECT_EQ(Arg(PlanFor("show my contacts"), "query"), std::nullopt);
  ExpectText(PlanFor("帮我找联系人 Tom"), "list_contacts", "query", "Tom", "search verb");
  ExpectText(PlanFor("通讯录里有没有张三"), "list_contacts", "query", "张三", "有没有");
  EXPECT_EQ(Arg(PlanFor("我的联系人都有谁"), "query"), std::nullopt);
}

TEST(LocalActionPlannerTest, PeopleToolsRenderAsAPeopleList) {
  const LocalPlan people = PlanFor("搜索用户 ken");
  const TurnPlan* p = AsPlan(people);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->render_mode, RenderMode::PeopleList);
  EXPECT_EQ(p->response_goal, ResponseGoal::PeopleDiscovery);
  const LocalPlan setting = PlanFor("关掉通知");
  const TurnPlan* q = AsPlan(setting);
  ASSERT_NE(q, nullptr);
  EXPECT_EQ(q->render_mode, RenderMode::Blocks);
}

// --- questions ----------------------------------------------------------------------------------------------

TEST(LocalActionPlannerTest, HowAndWhyQuestionsGoBackToTheStream) {
  const auto question = [](const LocalPlan& p, const std::string& what) {
    EXPECT_TRUE(std::holds_alternative<LocalQuestion>(p)) << what;
  };
  question(PlanAs("search_people", "我想了解怎么搜索用户", "搜索用户"), "我想了解怎么搜索用户");
  question(PlanFor("搜索用户怎么用？"), "搜索用户怎么用？");
  question(PlanFor("关通知怎么弄？"), "关通知怎么弄？");
  question(PlanAs("search_people", "how do I search for users", "search for users"), "how do I");
  question(PlanAs("set_notifications", "通知是什么？", "通知"), "通知是什么？");
  question(PlanFor("How do I add a contact?"), "How do I add a contact?");
  question(PlanFor("怎么加联系人"), "怎么加联系人");
}

TEST(LocalActionPlannerTest, PoliteRequestsAreCommands) {
  ExpectText(PlanAs("set_appearance", "可以打开深色模式吗？", "打开深色模式"), "set_appearance", "appearance", "dark", "可以…吗");
  ExpectBool(PlanAs("set_notifications", "能不能帮我把通知关掉", "关掉"), "set_notifications", "enabled", false, "能不能帮我");
  ExpectBool(PlanFor("能不能关掉通知"), "set_notifications", "enabled", false, "能不能 is not a negation");
  ExpectBool(PlanFor("Can you turn off notifications?"), "set_notifications", "enabled", false, "can you");
  ExpectBool(PlanFor("麻烦关掉通知"), "set_notifications", "enabled", false, "麻烦");
  ExpectBool(PlanFor("关掉通知？"), "set_notifications", "enabled", false, "a bare question mark");
}

TEST(LocalActionPlannerTest, SettingsReadSentencesWithWhatIsAreCommands) {
  ExpectPlan(PlanFor("我现在的设置是什么"), "get_preferences", "是什么");
  ExpectPlan(PlanFor("AI 用的是什么模型"), "get_llm_settings", "用的是什么模型");
}

// --- on / off -----------------------------------------------------------------------------------------------

TEST(LocalActionPlannerTest, OnAndOff) {
  ExpectBool(PlanFor("打开通知"), "set_notifications", "enabled", true, "打开通知");
  ExpectBool(PlanFor("关掉通知"), "set_notifications", "enabled", false, "关掉通知");
  ExpectBool(PlanFor("关通知"), "set_notifications", "enabled", false, "关通知");
  ExpectBool(PlanFor("开通知"), "set_notifications", "enabled", true, "开通知");
  ExpectBool(PlanAs("set_notifications", "通知关了", "通知关了"), "set_notifications", "enabled", false, "通知关了");
  ExpectBool(PlanFor("turn off notifications"), "set_notifications", "enabled", false, "turn off");
  ExpectBool(PlanFor("Please enable notifications"), "set_notifications", "enabled", true, "enable");
  ExpectBool(PlanFor("打开通知开关"), "set_notifications", "enabled", true, "开关 is a word, not a direction");
  ExpectBool(PlanFor("关闭通知开关"), "set_notifications", "enabled", false, "开关 again");
  ExpectBool(PlanFor("打开通话诊断"), "set_call_diagnostics", "enabled", true, "diagnostics");
  ExpectBool(PlanFor("打开自动续期"), "set_auto_renew_registration", "enabled", true, "auto renew");
  ExpectBool(PlanFor("打开节点模式"), "set_node_enabled", "enabled", true, "node mode");
  ExpectBool(PlanFor("turn on call diagnostics"), "set_call_diagnostics", "enabled", true, "diagnostics (en)");
}

TEST(LocalActionPlannerTest, OnOffThatIsNotClearIsAsked) {
  for (const auto& [text, hit] : std::vector<std::pair<std::string, std::string>>{
           {"通知呢", "通知"},
           {"别关通知", "关"},
           {"不要打开通知", "打开通知"},
           {"don't turn off notifications", "turn off notifications"},
           {"do not turn off notifications", "turn off notifications"},
           {"开启关闭通知的声音", "关闭通知"},
           {"通知的设置关于声音", "通知"},
           {"打开或者关闭通知", "通知"},
       }) {
    ExpectAsk(PlanAs("set_notifications", text, hit), "settings.ask.on_or_off", text);
  }
  EXPECT_EQ(std::get<LocalAsk>(PlanAs("set_notifications", "通知呢", "通知")).missing, "enabled");
}

TEST(LocalActionPlannerTest, BlurIsTheOppositeOfTheSetting) {
  ExpectBool(PlanFor("关掉毛玻璃"), "set_reduce_transparency", "enabled", true, "off blur = reduce");
  ExpectBool(PlanAs("set_reduce_transparency", "打开毛玻璃", "打开"), "set_reduce_transparency", "enabled", false, "on blur = not reduce");
  ExpectBool(PlanFor("reduce transparency"), "set_reduce_transparency", "enabled", true, "reduce");
  ExpectBool(PlanFor("turn off the blur"), "set_reduce_transparency", "enabled", true, "turn off the blur");
  ExpectBool(PlanAs("set_reduce_transparency", "关闭减少透明度", "减少透明度"), "set_reduce_transparency", "enabled", false, "off reduce");
}

// --- enumerations -------------------------------------------------------------------------------------------

TEST(LocalActionPlannerTest, Appearance) {
  ExpectText(PlanFor("换成深色模式"), "set_appearance", "appearance", "dark", "深色");
  ExpectText(PlanAs("set_appearance", "切换到浅色模式", "切换"), "set_appearance", "appearance", "light", "浅色");
  ExpectText(PlanFor("switch to dark mode"), "set_appearance", "appearance", "dark", "dark mode");
  ExpectText(PlanAs("set_appearance", "跟随系统外观", "跟随系统外观"), "set_appearance", "appearance", "system", "跟随系统");
  ExpectText(PlanFor("turn off dark mode"), "set_appearance", "appearance", "light", "off dark = light");
  ExpectAsk(PlanAs("set_appearance", "改外观", "改外观"), "settings.ask.appearance", "no value");
  ExpectAsk(PlanAs("set_appearance", "深色还是浅色模式", "模式"), "settings.ask.appearance", "both");
}

TEST(LocalActionPlannerTest, Language) {
  ExpectText(PlanAs("set_language", "切换到中文", "切换"), "set_language", "language", "zh-Hans", "中文");
  ExpectText(PlanFor("switch to English"), "set_language", "language", "en", "English");
  LocalPlanContext ctx;
  ctx.locales = {{"en", "English"}, {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"}};
  ExpectText(PlanFor("界面改成繁体中文", ctx), "set_language", "language", "zh-Hant", "traditional");
  ExpectText(PlanFor("界面改成中文", ctx), "set_language", "language", "zh-Hans", "中文 with two Chinese locales");
  ExpectAsk(PlanAs("set_language", "换个语言", "换个语言"), "settings.ask.language", "no value");
}

TEST(LocalActionPlannerTest, InvitePolicies) {
  ExpectText(PlanFor("谁都能拉我进群"), "set_group_invite_policy", "policy", "everyone", "谁都");
  ExpectText(PlanFor("只允许联系人邀请我"), "set_group_invite_policy", "policy", "contacts_only", "联系人");
  ExpectText(PlanFor("不让陌生人拉我进群"), "set_group_invite_policy", "policy", "contacts_only", "strangers");
  ExpectText(PlanAs("set_call_invite_policy", "不允许任何人打给我", "不允许"), "set_call_invite_policy", "policy", "nobody",
             "不允许任何人");
  ExpectText(PlanAs("set_call_invite_policy", "只让好友打给我", "只让"), "set_call_invite_policy", "policy", "contacts_only", "好友");
  ExpectText(PlanAs("set_call_invite_policy", "没人能打给我", "没人"), "set_call_invite_policy", "policy", "nobody", "没人");
  ExpectText(PlanAs("set_group_invite_policy", "let anyone add me to groups", "add me to groups"), "set_group_invite_policy",
             "policy", "everyone", "anyone");
  ExpectAsk(PlanAs("set_group_invite_policy", "改一下邀请策略", "邀请策略"), "settings.ask.policy", "no value");
  ExpectAsk(PlanAs("set_group_invite_policy", "任何人或者联系人", "联系人"), "settings.ask.policy", "two values");
}

TEST(LocalActionPlannerTest, MeshCapabilities) {
  ExpectBool(PlanAs("set_mesh_capabilities", "关掉媒体中继", "关掉"), "set_mesh_capabilities", "media_relay", false, "媒体中继");
  ExpectBool(PlanFor("关掉中继"), "set_mesh_capabilities", "circuit_relay", false, "中继");
  ExpectBool(PlanFor("打开 DHT"), "set_mesh_capabilities", "dht", true, "DHT");
  ExpectBool(PlanFor("turn off media relay"), "set_mesh_capabilities", "media_relay", false, "media relay");
  ExpectBool(PlanFor("turn on relay"), "set_mesh_capabilities", "circuit_relay", true, "relay");
  ExpectBool(PlanAs("set_mesh_capabilities", "打开优先联系人", "打开"), "set_mesh_capabilities", "prefer_contacts_for_routing", true,
             "prefer contacts");
  ExpectText(PlanAs("set_mesh_capabilities", "关掉直连", "关掉"), "set_mesh_capabilities", "direct_connections", "nobody", "direct off");
  ExpectAsk(PlanAs("set_mesh_capabilities", "打开直连", "打开"), "settings.ask.direct_audience", "direct on is not a flag");
  const LocalPlan both = PlanAs("set_mesh_capabilities", "同时开中继和 DHT", "中继");
  ExpectAsk(both, "settings.ask.mesh_capability", "two capabilities");
  EXPECT_EQ(std::get<LocalAsk>(both).missing, "capability");
  EXPECT_EQ(std::get<LocalAsk>(both).carry, "true");
  const LocalPlan nothing = PlanAs("set_mesh_capabilities", "改一下网络能力", "网络能力");
  ExpectAsk(nothing, "settings.ask.mesh_capability", "no capability");
  const LocalPlan no_direction = PlanAs("set_mesh_capabilities", "中继功能", "中继功能");
  ExpectAsk(no_direction, "settings.ask.on_or_off", "no direction");
  EXPECT_EQ(std::get<LocalAsk>(no_direction).carry, "circuit_relay");
}

TEST(LocalActionPlannerTest, UpnpOnlyWhenMentioned) {
  ExpectBool(PlanAs("probe_reachability", "探测一下连通性", "连通"), "probe_reachability", "try_upnp", false, "no upnp");
  ExpectBool(PlanAs("probe_reachability", "用 UPnP 探测", "探测"), "probe_reachability", "try_upnp", true, "upnp");
  ExpectBool(PlanFor("重新检测一下网络"), "probe_reachability", "try_upnp", false, "zh");
  ExpectBool(PlanFor("test my network with UPnP"), "probe_reachability", "try_upnp", true, "en");
}

// --- tools without parameters -------------------------------------------------------------------------------

TEST(LocalActionPlannerTest, ToolsWithoutParametersArePlannedAsTheyAre) {
  ExpectPlan(PlanFor("我现在的设置是什么"), "get_preferences", "get_preferences");
  ExpectPlan(PlanFor("支持哪些语言"), "list_locales", "list_locales");
  ExpectPlan(PlanFor("列出我的会话"), "list_conversations", "list_conversations");
  ExpectPlan(PlanFor("重置 AI 的权限"), "reset_tool_permissions", "reset");
  ExpectPlan(PlanFor("我的账号信息"), "get_profile_identity", "identity");
  ExpectPlan(PlanFor("What model are you using"), "get_llm_settings", "llm");
  EXPECT_EQ(Arg(PlanFor("我的账号信息"), "query"), std::nullopt);
}

TEST(LocalActionPlannerTest, ToolsWithoutAWordlistEntryAreUnsupported) {
  EXPECT_TRUE(std::holds_alternative<LocalUnsupported>(PlanAs("add_contact", "加张三为好友", "为好友")));
  EXPECT_TRUE(std::holds_alternative<LocalUnsupported>(PlanAs("no_such_tool", "x", "x")));
}

TEST(LocalActionPlannerTest, NoSpansMeansNoParameters) {
  // brief_AI handed the turn back and the wordlist did not hit.
  const AppActionMatch none{"search_people", {}};
  ExpectAsk(BuildLocalTurnPlan(none, "帮我看看 Alice 这个人", {}), "contacts.ask.who", "search_people");
  ExpectAsk(BuildLocalTurnPlan({"set_notifications", {}}, "那个通知", {}), "settings.ask.on_or_off", "bool");
  ExpectAsk(BuildLocalTurnPlan({"update_profile_nickname", {}}, "改成 Bob", {}), "profile.ask.nickname", "nickname");
  ExpectPlan(BuildLocalTurnPlan({"list_conversations", {}}, "我的聊天", {}), "list_conversations", "no-arg tool");
  EXPECT_TRUE(std::holds_alternative<LocalQuestion>(BuildLocalTurnPlan(none, "怎么搜索用户", {})));
}

// --- contacts -----------------------------------------------------------------------------------------------

TEST(LocalActionPlannerTest, ContactNotFound) {
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "", "Alice")};
  const LocalPlan p = PlanFor("打开和 Tom 的聊天", ctx);
  const auto* amb = std::get_if<LocalAmbiguous>(&p);
  ASSERT_NE(amb, nullptr);
  EXPECT_TRUE(amb->candidates.empty());
  EXPECT_EQ(amb->tool, "open_conversation");
}

TEST(LocalActionPlannerTest, OneContactOpensItsThread) {
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "", "Tom", 0, "thread-1"), Contact("c2", "", "Alice")};
  ExpectText(PlanFor("打开和 Tom 的聊天", ctx), "open_conversation", "thread_id", "thread-1", "打开和 Tom 的聊天");
  ExpectText(PlanFor("open the chat with tom", ctx), "open_conversation", "thread_id", "thread-1", "case-insensitive");
  ExpectText(PlanFor("打开和 Tom 的聊天，然后关掉通知", ctx), "open_conversation", "thread_id", "thread-1", "stop word");
  ExpectText(PlanFor("发消息给 Tom", ctx), "start_conversation", "contact_id", "c1", "start by name");
  ExpectText(PlanFor("给 Tom 发消息", ctx), "start_conversation", "contact_id", "c1", "name between the words");
  ExpectText(PlanFor("和 Tom 开个聊天", ctx), "start_conversation", "contact_id", "c1", "name before the phrase");
}

TEST(LocalActionPlannerTest, ContactWithoutAChatIsStarted) {
  LocalPlanContext ctx;
  LocalPlanContact c = Contact("c1", "", "Tom");
  c.thread_id.clear();
  ctx.contacts = {c};
  ExpectText(PlanFor("打开和 Tom 的聊天", ctx), "start_conversation", "contact_id", "c1", "no thread yet");
}

TEST(LocalActionPlannerTest, RemarkAndNicknameAreMergedAndDeduplicated) {
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "欠钱的老王", "Wang", 0, "t1"), Contact("c2", "", "Alice")};
  ExpectText(PlanFor("打开和 老王 的聊天", ctx), "open_conversation", "thread_id", "t1", "remark");
  ExpectText(PlanFor("打开和 Wang 的聊天", ctx), "open_conversation", "thread_id", "t1", "nickname");

  ctx.contacts = {Contact("c1", "Tom", "Tom", 0, "t1"), Contact("c2", "", "Alice")};
  ExpectText(PlanFor("打开和 Tom 的聊天", ctx), "open_conversation", "thread_id", "t1", "same name twice is one contact");

  ctx.contacts = {Contact("c1", "Tom-公司", "Tomas", 0, "t1"), Contact("c2", "", "Tom", 0, "t2")};
  const LocalPlan p = PlanFor("打开和 Tom 的聊天", ctx);
  const auto* amb = std::get_if<LocalAmbiguous>(&p);
  ASSERT_NE(amb, nullptr);
  EXPECT_EQ(amb->candidates.size(), 2u);
  EXPECT_FALSE(amb->identical_rows);
}

TEST(LocalActionPlannerTest, WholeWordBeatsPrefix) {
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "", "Tommy", 0, "t1"), Contact("c2", "", "Tom", 0, "t2")};
  ExpectText(PlanFor("打开和 Tom 的聊天", ctx), "open_conversation", "thread_id", "t2", "whole word");
  ctx.contacts = {Contact("c1", "", "Tommy", 0, "t1"), Contact("c2", "", "Alice")};
  ExpectText(PlanFor("打开和 Tom 的聊天", ctx), "open_conversation", "thread_id", "t1", "a single prefix hit");
}

TEST(LocalActionPlannerTest, PrefixHitsAreCapped) {
  LocalPlanContext ctx;
  for (int i = 0; i < static_cast<int>(kMaxPrefixCandidates); ++i) {
    ctx.contacts.push_back(Contact("c" + std::to_string(i), "", "Tomas" + std::to_string(i), i));
  }
  const LocalPlan five = PlanFor("打开和 Tom 的聊天", ctx);
  const auto* amb = std::get_if<LocalAmbiguous>(&five);
  ASSERT_NE(amb, nullptr);
  EXPECT_EQ(amb->candidates.size(), kMaxPrefixCandidates);
  ctx.contacts.push_back(Contact("c9", "", "Tomas9", 9));
  ExpectAsk(PlanFor("打开和 Tom 的聊天", ctx), "contacts.ask.which_contact", "six prefix hits");
}

TEST(LocalActionPlannerTest, IdenticalRowsAreFlagged) {
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "老王", "", 100), Contact("c2", "老王", "", 100)};
  const LocalPlan same = PlanFor("打开和 老王 的聊天", ctx);
  ASSERT_TRUE(std::holds_alternative<LocalAmbiguous>(same));
  EXPECT_TRUE(std::get<LocalAmbiguous>(same).identical_rows);
  EXPECT_EQ(std::get<LocalAmbiguous>(same).candidates.size(), 2u);

  ctx.contacts = {Contact("c1", "老王", "", 100), Contact("c2", "老王", "", 200)};
  const LocalPlan differ = PlanFor("打开和 老王 的聊天", ctx);
  ASSERT_TRUE(std::holds_alternative<LocalAmbiguous>(differ));
  EXPECT_FALSE(std::get<LocalAmbiguous>(differ).identical_rows) << "only the last-contact time differs";

  ctx.contacts = {Contact("c1", "老王", "Wang", 100), Contact("c2", "老王", "", 100)};
  EXPECT_FALSE(std::get<LocalAmbiguous>(PlanFor("打开和 老王 的聊天", ctx)).identical_rows) << "nickname differs";
}

TEST(LocalActionPlannerTest, NoNameOrAPronounIsAsked) {
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "", "Tom")};
  ExpectAsk(PlanAs("start_conversation", "给他发消息", "给他发消息", ctx), "contacts.ask.which_contact", "pronoun");
  ExpectAsk(PlanAs("open_conversation", "打开聊天", "打开聊天", ctx), "contacts.ask.which_contact", "no name");
}

// "直连 开": direct_connections takes an audience, so it is asked for and the answer is the value (no loop back to
// "which capability").
TEST(LocalActionPlannerTest, DirectConnectionsOnAsksForTheAudience) {
  const LocalPlan asked = PlanAs("set_mesh_capabilities", "直连 开", "直连");
  ExpectAsk(asked, "settings.ask.direct_audience", "直连 开");
  const LocalAsk ask = std::get<LocalAsk>(asked);
  EXPECT_EQ(ask.missing, "audience");
  for (const auto& [answer, audience] : std::vector<std::pair<std::string, std::string>>{
           {"任何人", "everyone"}, {"联系人", "contacts"}, {"好友", "friendly"}, {"没有人", "nobody"},
           {"everyone", "everyone"}, {"contacts", "contacts"}, {"friendly", "friendly"}, {"nobody", "nobody"},
           {"只让好友吧", "friendly"}}) {
    ExpectText(ResolvePendingAnswer(ask, answer, {}), "set_mesh_capabilities", "direct_connections", audience, answer);
  }
  ExpectAsk(ResolvePendingAnswer(ask, "开", {}), "settings.ask.direct_audience", "still no audience");
  // Named in the sentence: no question.
  ExpectText(PlanAs("set_mesh_capabilities", "打开直连 只对好友", "打开"), "set_mesh_capabilities", "direct_connections",
             "friendly", "audience in the sentence");
}

// --- follow-up answers --------------------------------------------------------------------------------------

TEST(LocalActionPlannerTest, CancelWordsInAShortAnswer) {
  const LocalAsk ask{"update_profile_nickname", "nickname", "profile.ask.nickname", ""};
  for (const char* answer : {"算了", "取消吧", "算了，不改了吧", "不用了", "别了", "never mind", "Cancel", "forget it"}) {
    EXPECT_TRUE(std::holds_alternative<LocalCancelled>(ResolvePendingAnswer(ask, answer, {}))) << answer;
  }
  // A cancel word with a value next to it is an answer, not a cancellation.
  ExpectText(ResolvePendingAnswer(ask, "不用了，叫Tom", {}), "update_profile_nickname", "nickname", "不用了，叫Tom",
             "不用了，叫Tom");
  ExpectText(ResolvePendingAnswer(ask, "cancel it, use Tom", {}), "update_profile_nickname", "nickname",
             "cancel it, use Tom", "cancel it, use Tom");
  EXPECT_TRUE(std::holds_alternative<LocalCancelled>(ResolvePendingAnswer(ask, "cancel it", {})));
  EXPECT_TRUE(std::holds_alternative<LocalCancelled>(ResolvePendingAnswer(ask, "算了吧！", {})));
  // Longer than the limit: it is a name that happens to contain a cancel word.
  const std::string long_answer = "取消的那个世界的另一个我的名字叫什么好呢";
  ExpectText(ResolvePendingAnswer(ask, long_answer, {}), "update_profile_nickname", "nickname", long_answer, "long answer");
}

TEST(LocalActionPlannerTest, TextAnswerIsTheWholeSentence) {
  const LocalAsk nick{"update_profile_nickname", "nickname", "profile.ask.nickname", ""};
  ExpectText(ResolvePendingAnswer(nick, "搜索达人", {}), "update_profile_nickname", "nickname", "搜索达人", "word-table words");
  ExpectText(ResolvePendingAnswer(nick, "Tony the Great", {}), "update_profile_nickname", "nickname", "Tony the Great", "english");
  EXPECT_TRUE(std::holds_alternative<LocalQuestion>(ResolvePendingAnswer(nick, "这个怎么用？", {})));
  EXPECT_TRUE(std::holds_alternative<LocalQuestion>(ResolvePendingAnswer(nick, "how do I do that", {})));
  ExpectAsk(ResolvePendingAnswer(nick, "   ", {}), "profile.ask.nickname", "empty");
  const LocalAsk query{"search_people", "query", "contacts.ask.who", ""};
  ExpectText(ResolvePendingAnswer(query, "Alice", {}), "search_people", "query", "Alice", "query");
}

TEST(LocalActionPlannerTest, BooleanAnswer) {
  const LocalAsk ask{"set_notifications", "enabled", "settings.ask.on_or_off", ""};
  ExpectBool(ResolvePendingAnswer(ask, "关掉", {}), "set_notifications", "enabled", false, "关掉");
  ExpectBool(ResolvePendingAnswer(ask, "off", {}), "set_notifications", "enabled", false, "off");
  ExpectBool(ResolvePendingAnswer(ask, "关", {}), "set_notifications", "enabled", false, "关");
  ExpectBool(ResolvePendingAnswer(ask, "开", {}), "set_notifications", "enabled", true, "开");
  ExpectBool(ResolvePendingAnswer(ask, "打开吧", {}), "set_notifications", "enabled", true, "打开吧");
  ExpectBool(ResolvePendingAnswer(ask, "开吧", {}), "set_notifications", "enabled", true, "开吧");
  ExpectBool(ResolvePendingAnswer(ask, "On", {}), "set_notifications", "enabled", true, "On");
  ExpectAsk(ResolvePendingAnswer(ask, "不开", {}), "settings.ask.on_or_off", "negated");
  ExpectAsk(ResolvePendingAnswer(ask, "随便", {}), "settings.ask.on_or_off", "unclear");
  EXPECT_TRUE(std::holds_alternative<LocalQuestion>(ResolvePendingAnswer(ask, "这个怎么用？", {})));
  EXPECT_TRUE(std::holds_alternative<LocalCancelled>(ResolvePendingAnswer(ask, "算了", {})));
  // The answer is about the setting itself, even for the blur setting.
  const LocalAsk blur{"set_reduce_transparency", "enabled", "settings.ask.on_or_off", ""};
  ExpectBool(ResolvePendingAnswer(blur, "关", {}), "set_reduce_transparency", "enabled", false, "blur answer");
}

TEST(LocalActionPlannerTest, EnumAnswers) {
  ExpectText(ResolvePendingAnswer({"set_appearance", "appearance", "settings.ask.appearance", ""}, "深色", {}), "set_appearance",
             "appearance", "dark", "appearance");
  ExpectText(ResolvePendingAnswer({"set_group_invite_policy", "policy", "settings.ask.policy", ""}, "只有联系人", {}),
             "set_group_invite_policy", "policy", "contacts_only", "policy");
  ExpectText(ResolvePendingAnswer({"set_language", "language", "settings.ask.language", ""}, "English", {}), "set_language",
             "language", "en", "language");
  ExpectAsk(ResolvePendingAnswer({"set_appearance", "appearance", "settings.ask.appearance", ""}, "嗯", {}),
            "settings.ask.appearance", "unclear");
}

TEST(LocalActionPlannerTest, MeshAnswers) {
  const LocalAsk which{"set_mesh_capabilities", "capability", "settings.ask.mesh_capability", "true"};
  ExpectBool(ResolvePendingAnswer(which, "DHT", {}), "set_mesh_capabilities", "dht", true, "capability, direction carried");
  const LocalAsk direction{"set_mesh_capabilities", "enabled", "settings.ask.on_or_off", "circuit_relay"};
  ExpectBool(ResolvePendingAnswer(direction, "关", {}), "set_mesh_capabilities", "circuit_relay", false, "direction, capability carried");
  const LocalAsk neither{"set_mesh_capabilities", "capability", "settings.ask.mesh_capability", ""};
  const LocalPlan half = ResolvePendingAnswer(neither, "DHT", {});
  ExpectAsk(half, "settings.ask.on_or_off", "capability without direction");
  EXPECT_EQ(std::get<LocalAsk>(half).carry, "dht");
}

TEST(LocalActionPlannerTest, ContactAnswer) {
  LocalPlanContext ctx;
  ctx.contacts = {Contact("c1", "", "Tom", 0, "t1")};
  const LocalAsk ask{"open_conversation", "contact", "contacts.ask.which_contact", ""};
  ExpectText(ResolvePendingAnswer(ask, "Tom", ctx), "open_conversation", "thread_id", "t1", "name");
  ExpectText(ResolvePendingAnswer(ask, "和 Tom 的聊天", ctx), "open_conversation", "thread_id", "t1", "name with filler");
  const LocalPlan none = ResolvePendingAnswer(ask, "Zed", ctx);
  ASSERT_TRUE(std::holds_alternative<LocalAmbiguous>(none));
  EXPECT_TRUE(std::get<LocalAmbiguous>(none).candidates.empty());
  ExpectAsk(ResolvePendingAnswer(ask, "他", ctx), "contacts.ask.which_contact", "pronoun");
}

TEST(LocalActionPlannerTest, WordTablesAreLowercaseAndNonEmpty) {
  const auto check = [](const auto& table, const char* name) {
    EXPECT_FALSE(table.empty()) << name;
    for (const std::string_view w : table) {
      EXPECT_FALSE(w.empty()) << name;
      for (const char c : w) {
        EXPECT_FALSE(c >= 'A' && c <= 'Z') << name << ": " << w;
      }
    }
  };
  check(kQuestionWordsZh, "kQuestionWordsZh");
  check(kQuestionWordsEn, "kQuestionWordsEn");
  check(kStopWordsZh, "kStopWordsZh");
  check(kActionFillerWords, "kActionFillerWords");
  check(kBoolOnWordsEn, "kBoolOnWordsEn");
  check(kCancelWords, "kCancelWords");
  check(kPolicyBlockWords, "kPolicyBlockWords");
}

// Device check 2026-10-28: the sentences that went to brief_AI, through the real classifier.
TEST(LocalActionPlannerTest, DeviceCheckSentences) {
  ExpectPlan(PlanFor("看一下我的设置"), "get_preferences", "看一下我的设置");
  ExpectPlan(PlanFor("我的网络状态"), "get_reachability", "我的网络状态");
  ExpectText(PlanFor("深色模式"), "set_appearance", "appearance", "dark", "深色模式");
  ExpectText(PlanFor("切换到中文"), "set_language", "language", "zh-Hans", "切换到中文");
  ExpectText(PlanFor("找一下 Alice"), "search_people", "query", "Alice", "找一下 Alice");
  ExpectText(PlanFor("搜索用户 ken"), "search_people", "query", "ken", "搜索用户 ken");
  ExpectText(PlanFor("look up Ken"), "search_people", "query", "Ken", "look up Ken");
}

// Simulator run 2026-10-28: a bare setting name asks on or off, and the next word answers.
TEST(LocalActionPlannerTest, ABareSettingNameAsksOnOrOff) {
  for (const char* text : {"通知呢", "Notifications?"}) {
    const LocalPlan p = PlanFor(text);
    ExpectAsk(p, "settings.ask.on_or_off", text);
    const auto* ask = std::get_if<LocalAsk>(&p);
    ASSERT_NE(ask, nullptr) << text;
    EXPECT_EQ(ask->tool, "set_notifications") << text;
    EXPECT_EQ(ask->missing, "enabled") << text;
    ExpectBool(ResolvePendingAnswer(*ask, std::string(text) == "通知呢" ? "关" : "off", {}), "set_notifications", "enabled",
               false, text);
  }
  ExpectAsk(PlanFor("透明度呢"), "settings.ask.on_or_off", "透明度呢");
  EXPECT_EQ(std::get<LocalAsk>(PlanFor("透明度呢")).tool, "set_reduce_transparency");
}
