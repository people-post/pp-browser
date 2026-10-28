// The local "operate the app" renderer (lib/ai/LocalActionRender.h) fed with what the REAL settings tools return.
// A tool that gains or renames a result key fails here until its `ai.card.<tool>.<key>` label exists.
#include "feature/settings/SettingsTools.h"

#include "foundation/data/BootstrapTypes.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/LocalActionRender.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::string UniquePath(const char* prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() / (std::string(prefix) + std::to_string(now))).string();
}

class LocalActionRenderSettingsTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    pbr::LocalizationService::Instance().SetPreferredLanguage(GetParam());
    pbr::SetLocalRenderStrictForTests(true); // a new unlabelled key of a real tool fails here

    config_path_ = UniquePath("pp_local_render_config_") + ".json";
    profile_dir_ = UniquePath("pp_local_render_profile_");
    data_dir_ = UniquePath("pp_local_render_data_");
    std::filesystem::create_directories(profile_dir_);
    std::filesystem::create_directories(data_dir_);
    pbr::BootstrapResult bootstrap{};
    bootstrap.config = pbr::Config::DefaultAppConfig();
    bootstrap.config.mcp_servers.clear();
    pbr::McpConfig mcp;
    mcp.id = "docs";
    mcp.enabled = true;
    bootstrap.config.mcp_servers.push_back(mcp);
    bootstrap.config_path = config_path_;
    bootstrap.data_dir = data_dir_;
    bootstrap.profile_data_dir = profile_dir_;
    bootstrap.profile_prefs = pbr::UserPreferences::DefaultProfile();
    store_.Initialize(std::move(bootstrap));
    pbr::RegisterSettingsTools(registry_, MakePorts());
  }

  void TearDown() override {
    pbr::SetLocalRenderStrictForTests(false);
    std::filesystem::remove(config_path_);
    std::filesystem::remove_all(profile_dir_);
    std::filesystem::remove_all(data_dir_);
  }

  pbr::SettingsToolPorts MakePorts() {
    pbr::SettingsToolPorts ports;
    ports.session_store = [this]() -> pbr::SessionStore& { return store_; };
    ports.load_profile_identity = []() {
      pbr::ProfileIdentityView view;
      view.ready = true;
      view.nickname = "tester";
      view.peer_id = "12D3KooWtest";
      view.registered = "yes";
      return view;
    };
    ports.available_locales = []() {
      return std::vector<pbr::LocaleInfo>{{.tag = "en", .native_name_key = "settings.language.en"},
                                          {.tag = "zh-Hans", .native_name_key = "settings.language.zh_hans"}};
    };
    ports.language_display_label = [](const std::string& tag) { return tag == "zh-Hans" ? "中文" : tag; };
    ports.apply_appearance = [](const std::string&) {};
    ports.load_pin_protection = []() { return pbr::PinProtectionView{.ready = true, .unlocked = true}; };
    ports.load_reachability = []() {
      pbr::SettingsReachabilityView view;
      view.status = pbr::SettingsReachabilityView::Status::OutboundOnly;
      return view;
    };
    ports.messaging_ready = []() { return true; };
    ports.last_mesh_error = []() { return std::string{}; };
    ports.run_reachability_probe = [](bool) {};
    return ports;
  }

  /** Runs the real tool; an error becomes {"success":false,"message":…} the way the executor hands it over. */
  pbr::Object Run(const std::string& tool, const pbr::Object& args = {}) {
    auto result = registry_.Execute(tool, args);
    if (!result) {
      pbr::Object failure;
      failure.set("success", false);
      failure.set("message", result.error().message);
      return failure;
    }
    auto parsed = pbr::TryParseObject(*result);
    EXPECT_TRUE(parsed.has_value()) << tool << ": " << *result;
    return parsed.value_or(pbr::Object{});
  }

  std::string config_path_;
  std::string profile_dir_;
  std::string data_dir_;
  pbr::SessionStore store_;
  pbr::ToolRegistry registry_;
};

INSTANTIATE_TEST_SUITE_P(Locales, LocalActionRenderSettingsTest, ::testing::Values("en", "zh-Hans"));

pbr::Object Args(const std::string& key, const std::string& value) {
  pbr::Object args;
  args.set(key, value);
  return args;
}

pbr::Object BoolArgs(const bool enabled) {
  pbr::Object args;
  args.set("enabled", enabled);
  return args;
}

bool Clean(const std::string& text) {
  return !text.empty() && text.find("??") == std::string::npos && text.find('{') == std::string::npos &&
         text.rfind("ai.", 0) == std::string::npos;
}

} // namespace

TEST_P(LocalActionRenderSettingsTest, EveryReaderToolRendersAsALabelledCard) {
  const char* readers[] = {"get_profile_identity", "get_preferences", "list_locales",        "get_reachability",
                           "get_security_status",  "get_network_settings", "get_llm_settings", "get_integrations"};
  for (const char* tool : readers) {
    const pbr::Object result = Run(tool);
    ASSERT_FALSE(result.empty()) << tool;
    const std::string text = pbr::RenderLocalResult(tool, result, {});
    EXPECT_EQ(text.find("??"), std::string::npos) << tool << ":\n" << text;
    EXPECT_EQ(text.find("ai.card"), std::string::npos) << tool << ":\n" << text;
    size_t lines = 1;
    for (const char c : text) {
      lines += c == '\n' ? 1 : 0;
    }
    EXPECT_EQ(lines, result.size()) << tool << ": one line per key\n" << text;
    for (const auto& [key, value] : result.fields()) {
      if (key.find('_') != std::string::npos) {
        EXPECT_EQ(text.find(key), std::string::npos) << tool << " shows raw key " << key << ":\n" << text;
      }
    }
  }
}

TEST_P(LocalActionRenderSettingsTest, CardValuesAreReadable) {
  const std::string prefs = pbr::RenderLocalResult("get_preferences", Run("get_preferences"), {});
  EXPECT_EQ(prefs.find("true"), std::string::npos) << prefs;
  EXPECT_EQ(prefs.find("false"), std::string::npos) << prefs;
  const std::string reach = pbr::RenderLocalResult("get_reachability", Run("get_reachability"), {});
  EXPECT_EQ(reach.find("outbound_only"), std::string::npos) << reach;
}

TEST_P(LocalActionRenderSettingsTest, EveryWriterToolRendersASentence) {
  struct Case {
    const char* tool;
    pbr::Object args;
    const char* must_not_contain; // the raw value must have become a label
  };
  const std::vector<Case> cases = {
      {"set_appearance", Args("appearance", "dark"), "dark"},
      {"set_language", Args("language", "zh-Hans"), "zh-Hans"},
      {"set_notifications", BoolArgs(false), "false"},
      {"set_notifications", BoolArgs(true), "true"},
      {"set_group_invite_policy", Args("policy", "contacts_only"), "contacts_only"},
      {"set_call_invite_policy", Args("policy", "nobody"), "nobody"},
      {"set_auto_renew_registration", BoolArgs(true), "true"},
      {"set_reduce_transparency", BoolArgs(true), "true"},
      {"set_call_diagnostics", BoolArgs(false), "false"},
      {"set_node_enabled", BoolArgs(true), "true"},
      {"probe_reachability", pbr::Object{}, "outbound_only"},
      {"reset_tool_permissions", pbr::Object{}, "tool_permissions"},
  };
  for (const Case& c : cases) {
    const pbr::Object result = Run(c.tool, c.args);
    ASSERT_NE(result.getIf<bool>("success"), std::optional<bool>(false)) << c.tool << ": "
                                                                         << result.getString("message").value_or("");
    pbr::LocalRenderContext ctx;
    ctx.args = c.args;
    const std::string text = pbr::RenderLocalResult(c.tool, result, ctx);
    EXPECT_TRUE(Clean(text)) << c.tool << ": " << text;
    EXPECT_EQ(text.find(c.must_not_contain), std::string::npos) << c.tool << ": " << text;
  }
  // on and off must read differently
  EXPECT_NE(pbr::RenderLocalResult("set_notifications", Run("set_notifications", BoolArgs(true)), {}),
            pbr::RenderLocalResult("set_notifications", Run("set_notifications", BoolArgs(false)), {}));
}

TEST_P(LocalActionRenderSettingsTest, MeshCapabilitiesRenders) {
  pbr::Object args;
  args.set("circuit_relay", true);
  const pbr::Object result = Run("set_mesh_capabilities", args);
  EXPECT_TRUE(Clean(pbr::RenderLocalResult("set_mesh_capabilities", result, {})));
}

TEST_P(LocalActionRenderSettingsTest, ToolFailureIsWordedByTheTemplate) {
  const pbr::Object failure = Run("set_appearance", Args("appearance", "plaid"));
  ASSERT_EQ(failure.getIf<bool>("success"), std::optional<bool>(false));
  const std::string text = pbr::RenderLocalResult("set_appearance", failure, {});
  EXPECT_EQ(text, pbr::Tr("ai.action_failed"));
  EXPECT_NE(text, failure.getString("message").value()); // the tool's own (English, internal) text is not shown
}
