#include "lib/ai/LocalActionRender.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/LocalActionWords.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string AssetsRoot() { return std::string(PP_BROWSER_SOURCE_ROOT) + "/assets"; }

Object Parse(const std::string& json) {
  auto parsed = TryParseObject(json);
  EXPECT_TRUE(parsed.has_value()) << json;
  return parsed.value_or(Object{});
}

bool HasUnfilled(const std::string& text) {
  return text.find('{') != std::string::npos || text.find("??") != std::string::npos;
}

class LocalActionRenderTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(AssetsRoot()));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
    SetLocalRenderStrictForTests(true); // an unlabelled card key fails these tests instead of vanishing
  }
  void TearDown() override { SetLocalRenderStrictForTests(false); }
};

INSTANTIATE_TEST_SUITE_P(Locales, LocalActionRenderTest, ::testing::Values("en", "zh-Hans"));

LocalAsk MakeAsk(const std::string& tool, const std::string& key) {
  LocalAsk ask;
  ask.tool = tool;
  ask.question_key = key;
  return ask;
}

} // namespace

TEST_P(LocalActionRenderTest, EveryQuestionKeyHasText) {
  const std::vector<LocalAsk> asks = {
      MakeAsk("search_people", "contacts.ask.who"),
      MakeAsk("open_conversation", "contacts.ask.which_contact"),
      MakeAsk("update_profile_nickname", "profile.ask.nickname"),
      MakeAsk("update_profile_nickname", "profile.ask.nickname_confirm"),
      MakeAsk("set_notifications", "settings.ask.on_or_off"),
      MakeAsk("set_reduce_transparency", "settings.ask.on_or_off"),
      MakeAsk("set_auto_renew_registration", "settings.ask.on_or_off"),
      MakeAsk("set_call_diagnostics", "settings.ask.on_or_off"),
      MakeAsk("set_node_enabled", "settings.ask.on_or_off"),
      MakeAsk("set_mesh_capabilities", "settings.ask.on_or_off"),
      MakeAsk("set_appearance", "settings.ask.appearance"),
      MakeAsk("set_language", "settings.ask.language"),
      MakeAsk("set_group_invite_policy", "settings.ask.policy"),
      MakeAsk("set_mesh_capabilities", "settings.ask.mesh_capability"),
  };
  for (const LocalAsk& ask : asks) {
    const std::string text = RenderLocalAsk(ask);
    EXPECT_FALSE(text.empty()) << ask.question_key;
    EXPECT_NE(text, ask.question_key) << ask.tool;
    EXPECT_FALSE(HasUnfilled(text)) << text;
  }
}

TEST_P(LocalActionRenderTest, OnOffQuestionNamesTheSetting) {
  const std::string generic = RenderLocalAsk(MakeAsk("set_mesh_capabilities", "settings.ask.on_or_off"));
  for (const char* tool : {"set_notifications", "set_reduce_transparency", "set_auto_renew_registration",
                           "set_call_diagnostics", "set_node_enabled"}) {
    EXPECT_NE(RenderLocalAsk(MakeAsk(tool, "settings.ask.on_or_off")), generic) << tool;
  }
  const std::string transparency = RenderLocalAsk(MakeAsk("set_reduce_transparency", "settings.ask.on_or_off"));
  EXPECT_NE(transparency.find(GetParam() == std::string("en") ? "transparency" : "透明度"), std::string::npos);
}

TEST_P(LocalActionRenderTest, FixedTextsExist) {
  for (const std::string& text : {RenderLocalCancelled(), RenderLocalGaveUp(), RenderLocalUnsupported(),
                                  RenderLocalNotShareable(), RenderLocalContactGone()}) {
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.find("ai."), std::string::npos) << text;
  }
  EXPECT_EQ(RenderLocalCancelled(), GetParam() == std::string("en") ? "OK, leaving it as is." : "好，先不动。");
}

TEST_P(LocalActionRenderTest, WriterSentencesCarryTheirValues) {
  LocalRenderContext ctx;
  auto render = [&](const std::string& tool, const std::string& json) { return RenderLocalResult(tool, Parse(json), ctx); };

  ctx.args = Parse(R"({"nickname":"Bob"})");
  const std::string nick = render("update_profile_nickname", R"({"success":true,"message":"ok"})");
  EXPECT_NE(nick.find("Bob"), std::string::npos) << nick;

  ctx.name = "Old Wang";
  for (const char* tool : {"open_conversation", "start_conversation"}) {
    const std::string opened = render(tool, R"({"id":"t1"})");
    EXPECT_NE(opened.find("Old Wang"), std::string::npos) << opened;
    EXPECT_FALSE(HasUnfilled(opened)) << opened;
  }

  const std::string dark = render("set_appearance", R"({"success":true,"appearance":"dark"})");
  EXPECT_NE(dark.find(GetParam() == std::string("en") ? "Dark" : "深色"), std::string::npos) << dark;

  const std::string zh = render("set_language", R"({"success":true,"language":"zh-Hans"})");
  EXPECT_NE(zh.find("简体中文"), std::string::npos) << zh;

  const std::string policy = render("set_group_invite_policy", R"({"success":true,"group_invite_policy":"nobody"})");
  EXPECT_FALSE(HasUnfilled(policy)) << policy;
  EXPECT_EQ(policy.find("nobody"), std::string::npos) << policy;
  EXPECT_FALSE(render("set_call_invite_policy", R"({"success":true,"call_invite_policy":"contacts_only"})").empty());

  const std::string on = render("set_notifications", R"({"success":true,"show_notifications":true})");
  const std::string off = render("set_notifications", R"({"success":true,"show_notifications":false})");
  EXPECT_NE(on, off);
  EXPECT_FALSE(HasUnfilled(on)) << on;

  for (const char* tool : {"register_user", "set_mesh_capabilities", "reset_tool_permissions", "probe_reachability"}) {
    const std::string text = render(tool, R"({"success":true,"status":"reachable"})");
    EXPECT_FALSE(text.empty()) << tool;
    EXPECT_FALSE(HasUnfilled(text)) << text;
    EXPECT_EQ(text.rfind("ai.", 0), std::string::npos) << text;
  }
}

TEST_P(LocalActionRenderTest, FailureNeverShowsTheRawMessage) {
  const std::string text = RenderLocalResult(
      "set_appearance", Parse(R"({"success":false,"message":"appearance must be system, light, or dark"})"), {});
  EXPECT_EQ(text, Tr("ai.action_failed"));
  EXPECT_EQ(text.find("appearance must"), std::string::npos);

  const std::string blocked = RenderLocalResult(
      "set_notifications",
      Parse(R"({"success":false,"blocked":true,"message":"Tool 'set_notifications' is blocked by your tool permission settings."})"),
      {});
  EXPECT_EQ(blocked, Tr("ai.blocked_by_permissions"));
  EXPECT_EQ(blocked.find("Tool '"), std::string::npos);
  EXPECT_FALSE(HasUnfilled(blocked));
}

TEST_P(LocalActionRenderTest, CardLabelsEveryKeyAndFlagsMissingOnes) {
  const std::string card = RenderLocalResult(
      "get_network_settings",
      Parse(R"({"node_enabled":true,"amp_udp_port":4001,"direct_connections":"contacts","circuit_relay":false})"), {});
  EXPECT_EQ(card.find("??"), std::string::npos) << card;
  EXPECT_EQ(card.find("node_enabled"), std::string::npos) << card;
  EXPECT_NE(card.find("4001"), std::string::npos);
  EXPECT_EQ(std::count(card.begin(), card.end(), '\n'), 3);

  const std::string flagged = RenderLocalResult("get_preferences", Parse(R"({"brand_new_key":1})"), {});
  EXPECT_EQ(flagged, "??brand_new_key??");

  // At runtime an unlabelled key is left out, never shown raw.
  SetLocalRenderStrictForTests(false);
  const std::string lenient =
      RenderLocalResult("get_preferences", Parse(R"({"appearance":"dark","brand_new_key":1})"), {});
  EXPECT_EQ(lenient.find("brand_new_key"), std::string::npos) << lenient;
  EXPECT_EQ(lenient.find("??"), std::string::npos) << lenient;
  EXPECT_EQ(lenient.find('\n'), std::string::npos) << "one line, no blank one: " << lenient;
  EXPECT_FALSE(lenient.empty());
}

TEST_P(LocalActionRenderTest, CardShowsListsAndEmptyValues) {
  const std::string card = RenderLocalResult(
      "get_integrations", Parse(R"({"search_provider":"","mcp_servers":[{"id":"a","enabled":true,"kind":"custom"}]})"), {});
  EXPECT_EQ(card.find("??"), std::string::npos) << card;
  EXPECT_NE(card.find("a ("), std::string::npos) << card;
}

TEST_P(LocalActionRenderTest, OnlyFirstActionHintIsAppended) {
  LocalRenderContext ctx;
  const std::string plain = RenderLocalResult("set_appearance", Parse(R"({"success":true,"appearance":"dark"})"), ctx);
  ctx.synthesis_hints = std::string(kHintOnlyFirstAction);
  const std::string hinted = RenderLocalResult("set_appearance", Parse(R"({"success":true,"appearance":"dark"})"), ctx);
  EXPECT_EQ(hinted.rfind(plain, 0), 0u);
  EXPECT_GT(hinted.size(), plain.size());
  EXPECT_EQ(hinted.find("ai.only_first_action"), std::string::npos);
}

TEST_P(LocalActionRenderTest, AmbiguousListsOneOpenRowPerCandidate) {
  LocalAmbiguous amb;
  amb.tool = "open_conversation";
  amb.candidates = {{.id = "c1", .thread_id = "t1", .remark = "Old Wang (debt)", .nickname = "Wang", .last_contact_ms = 1700000000000},
                    {.id = "c2", .thread_id = "", .remark = "", .nickname = "Wang", .last_contact_ms = 0}};
  const Object root = Parse(RenderLocalAmbiguous(amb));
  const Array* blocks = root.getArray("blocks");
  ASSERT_NE(blocks, nullptr);
  const Object* list = nullptr;
  for (const Value& b : blocks->elements) {
    if (const Object* o = asObject(b); o && o->getString("type") == std::optional<std::string>("long_list")) {
      list = o;
    }
  }
  ASSERT_NE(list, nullptr);
  const Array* items = list->getArray("items");
  ASSERT_NE(items, nullptr);
  ASSERT_EQ(items->elements.size(), 2u);

  const Object* first = asObject(items->elements[0]);
  EXPECT_EQ(first->getString("title"), std::optional<std::string>("Old Wang (debt)"));
  EXPECT_EQ(first->getString("subtitle"), std::optional<std::string>("~Wang"));
  EXPECT_TRUE(first->getString("meta").has_value()); // last contact date
  const Object* open = asObject(first->getArray("actions")->elements[0]);
  const Object* payload = open->getObject("payload");
  EXPECT_EQ(payload->getString("type"), std::optional<std::string>("local_action"));
  EXPECT_EQ(payload->getString("tool"), std::optional<std::string>("open_conversation"));
  EXPECT_EQ(payload->getString("contact_id"), std::optional<std::string>("c1"));
  EXPECT_EQ(payload->getString("thread_id"), std::optional<std::string>("t1"));

  const Object* second = asObject(items->elements[1]);
  EXPECT_EQ(second->getString("title"), std::optional<std::string>("Wang"));
  EXPECT_FALSE(second->getString("meta").has_value());
  const Object* start = asObject(second->getArray("actions")->elements[0])->getObject("payload");
  EXPECT_EQ(start->getString("tool"), std::optional<std::string>("start_conversation"));
  EXPECT_EQ(start->getString("contact_id"), std::optional<std::string>("c2"));
}

TEST_P(LocalActionRenderTest, IdenticalRowsAskForARemarkAndNeverOpen) {
  LocalAmbiguous amb;
  amb.tool = "open_conversation";
  amb.identical_rows = true;
  amb.candidates = {{.id = "c1", .thread_id = "t1", .remark = "Old Wang", .nickname = "Wang"},
                    {.id = "c2", .thread_id = "t2", .remark = "Old Wang", .nickname = "Wang"}};
  const std::string json = RenderLocalAmbiguous(amb);
  EXPECT_EQ(json.find("open_conversation"), std::string::npos) << json;
  EXPECT_EQ(json.find("start_conversation"), std::string::npos) << json;
  const Object root = Parse(json);
  const Object* list = asObject(root.getArray("blocks")->elements[1]);
  const Array* items = list->getArray("items");
  ASSERT_EQ(items->elements.size(), 2u);
  const std::string needs = Tr("ai.contact.needs_remark");
  for (size_t i = 0; i < 2; ++i) {
    const Object* item = asObject(items->elements[i]);
    EXPECT_EQ(item->getString("meta"), std::optional<std::string>(needs));
    const Object* payload = asObject(item->getArray("actions")->elements[0])->getObject("payload");
    EXPECT_EQ(payload->getString("tool"), std::optional<std::string>("edit_remark"));
    EXPECT_EQ(payload->getString("contact_id"), std::optional<std::string>(i == 0 ? "c1" : "c2"));
  }
}

TEST_P(LocalActionRenderTest, NoCandidateOffersADirectorySearch) {
  LocalAmbiguous amb;
  amb.tool = "open_conversation";
  const std::string json = RenderLocalAmbiguous(amb, "Alice");
  const Object root = Parse(json);
  const Array* blocks = root.getArray("blocks");
  ASSERT_EQ(blocks->elements.size(), 2u);
  EXPECT_EQ(asObject(blocks->elements[0])->getString("text"), std::optional<std::string>(Tr("ai.contact.not_found")));
  const Object* item = asObject(asObject(blocks->elements[1])->getArray("items")->elements[0]);
  const Object* payload = asObject(item->getArray("actions")->elements[0])->getObject("payload");
  EXPECT_EQ(payload->getString("tool"), std::optional<std::string>("search_people"));
  EXPECT_EQ(payload->getString("query"), std::optional<std::string>("Alice"));
}

TEST_P(LocalActionRenderTest, OptionLabelsExistForEveryKnownValue) {
  const std::vector<std::pair<std::string, std::vector<std::string>>> groups = {
      {"appearance", {"system", "light", "dark"}},
      {"policy", {"everyone", "contacts_only", "nobody"}},
      {"status", {"unknown", "checking", "reachable", "outbound_only", "blocked"}},
      {"pin_status", {"not_setup", "app_default", "custom"}},
      {"direct_connections", {"everyone", "contacts", "friendly", "nobody"}},
      {"preset", {"brief", "cloud", "ollama", "custom"}},
      {"api_key", {"set", "env", "unset"}},
      {"kind", {"promoted", "custom"}},
  };
  for (const auto& [group, values] : groups) {
    for (const std::string& value : values) {
      const std::string key = "ai.option." + group + "." + value;
      EXPECT_NE(Tr(key), key);
    }
  }
}

// Every key the render step added exists in both catalogs with the same {placeholders}.
TEST(LocalActionRenderLocaleTest, NewKeysExistInBothLocalesWithSamePlaceholders) {
  const auto load = [](const char* tag) {
    const Object doc = Parse(ReadFile(AssetsRoot() + "/locales/" + tag + ".json"));
    const Object* strings = doc.getObject("strings");
    EXPECT_NE(strings, nullptr);
    std::map<std::string, std::string> out;
    if (strings) {
      for (const auto& [key, value] : strings->fields()) {
        if (const auto* s = std::get_if<std::string>(&value)) {
          out[key] = *s;
        }
      }
    }
    return out;
  };
  const auto placeholders = [](const std::string& text) {
    std::set<std::string> names;
    for (size_t open = text.find('{'); open != std::string::npos; open = text.find('{', open + 1)) {
      const size_t close = text.find('}', open);
      if (close == std::string::npos) {
        break;
      }
      names.insert(text.substr(open, close - open + 1));
    }
    return names;
  };
  const auto en = load("en");
  const auto zh = load("zh-Hans");
  const std::vector<std::string> prefixes = {"ai.", "contacts.ask.", "profile.ask.", "settings.ask."};
  size_t checked = 0;
  for (const auto& [key, text] : en) {
    for (const std::string& prefix : prefixes) {
      if (key.rfind(prefix, 0) != 0) {
        continue;
      }
      ++checked;
      ASSERT_TRUE(zh.count(key)) << "missing in zh-Hans: " << key;
      EXPECT_EQ(placeholders(text), placeholders(zh.at(key))) << key;
      EXPECT_FALSE(text.empty()) << key;
    }
  }
  for (const auto& [key, text] : zh) {
    for (const std::string& prefix : prefixes) {
      if (key.rfind(prefix, 0) == 0) {
        EXPECT_TRUE(en.count(key)) << "missing in en: " << key;
      }
    }
  }
  EXPECT_GT(checked, 100u);
}

} // namespace pbr
