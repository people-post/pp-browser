#include "feature/settings/SettingsTools.h"

#include "base/data/BootstrapTypes.h"
#include "base/data/Config.h"
#include "base/data/SessionStore.h"
#include "base/data/UserPreferences.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::string UniquePath(const char* prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() / (std::string(prefix) + std::to_string(now))).string();
}

class SettingsToolsTest : public ::testing::Test {
protected:
  void SetUp() override {
    config_path_ = UniquePath("pp_settings_tools_config_") + ".json";
    profile_dir_ = UniquePath("pp_settings_tools_profile_");
    data_dir_ = UniquePath("pp_settings_tools_data_");
    std::filesystem::create_directories(profile_dir_);
    std::filesystem::create_directories(data_dir_);

    pbr::BootstrapResult bootstrap{};
    bootstrap.config = pbr::Config::DefaultAppConfig();
    bootstrap.config_path = config_path_;
    bootstrap.data_dir = data_dir_;
    bootstrap.profile_data_dir = profile_dir_;
    bootstrap.profile_prefs = pbr::UserPreferences::DefaultProfile();
    store_.Initialize(std::move(bootstrap));
  }

  void TearDown() override {
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
    ports.apply_appearance = [this](const std::string& appearance) { last_appearance_ = appearance; };
    ports.load_pin_protection = []() { return pbr::PinProtectionView{.ready = true, .unlocked = true}; };
    ports.load_reachability = []() {
      pbr::SettingsReachabilityView view;
      view.status = pbr::SettingsReachabilityView::Status::Reachable;
      return view;
    };
    ports.messaging_ready = []() { return true; };
    ports.last_libp2p_error = []() { return std::string{}; };
    ports.run_reachability_probe = [this](bool try_upnp) {
      probed_ = true;
      try_upnp_ = try_upnp;
    };
    return ports;
  }

  std::string config_path_;
  std::string profile_dir_;
  std::string data_dir_;
  pbr::SessionStore store_;
  std::string last_appearance_;
  bool probed_ = false;
  bool try_upnp_ = false;
};

} // namespace

TEST_F(SettingsToolsTest, RegistersReadAndWriteTools) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  bool saw_get_prefs = false;
  bool saw_set_appearance = false;
  for (const pbr::ToolDescriptor& tool : registry.Tools()) {
    EXPECT_EQ(tool.meta.provider, "settings");
    if (tool.definition.name == "get_preferences") {
      saw_get_prefs = true;
      EXPECT_FALSE(tool.meta.mutating);
      EXPECT_EQ(tool.meta.risk, "read");
    }
    if (tool.definition.name == "set_appearance") {
      saw_set_appearance = true;
      EXPECT_TRUE(tool.meta.mutating);
      EXPECT_EQ(tool.meta.risk, "write");
    }
  }
  EXPECT_TRUE(saw_get_prefs);
  EXPECT_TRUE(saw_set_appearance);
  EXPECT_GE(registry.Tools().size(), 15u);
}

TEST_F(SettingsToolsTest, GetAndSetAppearance) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  auto before = registry.Execute("get_preferences", nlohmann::json::object());
  ASSERT_TRUE(before);

  auto set = registry.Execute("set_appearance", nlohmann::json{{"appearance", "dark"}});
  ASSERT_TRUE(set) << set.error().message;
  EXPECT_EQ(last_appearance_, "dark");
  EXPECT_EQ(store_.Snapshot().profile_prefs.appearance, "dark");
}

TEST_F(SettingsToolsTest, SetAppearanceAcceptsThemeAliasAndCase) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  auto set = registry.Execute("set_appearance", nlohmann::json{{"theme", "Dark"}});
  ASSERT_TRUE(set) << set.error().message;
  EXPECT_EQ(last_appearance_, "dark");
  EXPECT_EQ(store_.Snapshot().profile_prefs.appearance, "dark");
}

TEST_F(SettingsToolsTest, SetAppearanceAcceptsDarkModeBool) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  auto set = registry.Execute("set_appearance", nlohmann::json{{"dark_mode", true}});
  ASSERT_TRUE(set) << set.error().message;
  EXPECT_EQ(last_appearance_, "dark");
}

TEST_F(SettingsToolsTest, SetLanguageValidatesLocales) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  auto bad = registry.Execute("set_language", nlohmann::json{{"language", "xx-YY"}});
  EXPECT_FALSE(bad);

  auto ok = registry.Execute("set_language", nlohmann::json{{"language", "zh-Hans"}});
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(store_.Snapshot().profile_prefs.language, "zh-Hans");
}

TEST_F(SettingsToolsTest, SetLanguageAcceptsLocaleAliasAndTagVariants) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  auto via_locale = registry.Execute("set_language", nlohmann::json{{"locale", "zh"}});
  ASSERT_TRUE(via_locale) << via_locale.error().message;
  EXPECT_EQ(store_.Snapshot().profile_prefs.language, "zh-Hans");

  auto via_cn = registry.Execute("set_language", nlohmann::json{{"lang", "zh-CN"}});
  ASSERT_TRUE(via_cn) << via_cn.error().message;
  EXPECT_EQ(store_.Snapshot().profile_prefs.language, "zh-Hans");

  auto via_label = registry.Execute("set_language", nlohmann::json{{"language", "中文"}});
  ASSERT_TRUE(via_label) << via_label.error().message;
  EXPECT_EQ(store_.Snapshot().profile_prefs.language, "zh-Hans");

  auto via_english = registry.Execute("set_language", nlohmann::json{{"language", "English"}});
  ASSERT_TRUE(via_english) << via_english.error().message;
  EXPECT_EQ(store_.Snapshot().profile_prefs.language, "en");

  auto via_system = registry.Execute("set_language", nlohmann::json{{"language", "System"}});
  ASSERT_TRUE(via_system) << via_system.error().message;
  EXPECT_EQ(store_.Snapshot().profile_prefs.language, "system");
}

TEST_F(SettingsToolsTest, SetNotificationsAcceptsStringBoolAndAlias) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  auto set = registry.Execute("set_notifications", nlohmann::json{{"show_notifications", "false"}});
  ASSERT_TRUE(set) << set.error().message;
  EXPECT_FALSE(store_.Snapshot().profile_prefs.show_notifications);
}

TEST_F(SettingsToolsTest, SetGroupInvitePolicyAcceptsAliases) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());

  auto set =
      registry.Execute("set_group_invite_policy", nlohmann::json{{"group_invite_policy", "Contacts Only"}});
  ASSERT_TRUE(set) << set.error().message;
  EXPECT_EQ(store_.Snapshot().profile_prefs.group_invite_policy, "contacts_only");
}

TEST_F(SettingsToolsTest, ResetToolPermissionsClearsRemembered) {
  pbr::ProfilePreferences prefs = store_.Snapshot().profile_prefs;
  prefs.tool_permissions.by_tool["add_contact"] = {.decision = "allow"};
  prefs.schema_version = pbr::ProfilePreferences::kSchemaVersion;
  ASSERT_TRUE(store_.SaveProfilePrefs(prefs));

  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());
  auto reset = registry.Execute("reset_tool_permissions", nlohmann::json::object());
  ASSERT_TRUE(reset) << reset.error().message;
  EXPECT_TRUE(store_.Snapshot().profile_prefs.tool_permissions.by_tool.empty());
}

TEST_F(SettingsToolsTest, ProbeReachabilityUsesPort) {
  pbr::ToolRegistry registry;
  pbr::RegisterSettingsTools(registry, MakePorts());
  auto result = registry.Execute("probe_reachability", nlohmann::json{{"try_upnp", true}});
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(probed_);
  EXPECT_TRUE(try_upnp_);
}
