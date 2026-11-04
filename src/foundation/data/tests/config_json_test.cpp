#include "foundation/data/ConfigJson.h"
#include "common/ValueJson.h"
#include "foundation/platform/ICredentialStore.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

TEST(ConfigJsonTest, ParsesLegacyAndModernFields) {
  auto legacy = pbr::TryParseObject(R"({
    "mcp": { "url": "https://legacy.example/mcp" },
    "mcp_servers": [
      { "id": "custom-a", "url": "https://custom.example/mcp" }
    ]
  })");
  ASSERT_TRUE(legacy.has_value());

  pbr::AppConfig config;
  pbr::AppConfigFromObject(*legacy, config);
  EXPECT_EQ(config.promoted_mcp.url, "https://legacy.example/mcp");
  ASSERT_EQ(config.mcp_servers.size(), 1u);
  EXPECT_EQ(config.mcp_servers[0].id, "custom-a");

  auto modern = pbr::TryParseObject(R"({
    "promoted_mcp": { "url": "https://promoted.example/mcp" },
    "mcp_servers": [
      { "id": "b", "command": "mock", "enabled": true }
    ],
    "relay": { "base_url": "https://relay.example" },
    "directory": { "base_url": "" },
    "registration": { "base_url": "" }
  })");
  ASSERT_TRUE(modern.has_value());

  pbr::AppConfigFromObject(*modern, config);
  EXPECT_EQ(config.promoted_mcp.url, "https://promoted.example/mcp");
  ASSERT_EQ(config.mcp_servers.size(), 1u);
  EXPECT_EQ(config.mcp_servers[0].command, "mock");
  EXPECT_EQ(config.relay.base_url, "https://relay.example");
}

TEST(ConfigJsonTest, EmitsModernConfigKeys) {
  pbr::AppConfig config;
  config.promoted_mcp.url = "https://promoted.example/mcp";

  const pbr::Object out = pbr::AppConfigToObject(config);
  EXPECT_TRUE(out.contains("promoted_mcp"));
  EXPECT_FALSE(out.contains("mcp"));
}

TEST(ConfigJsonTest, ResolvesPromotedMcpFallbacks) {
  pbr::AppConfig defaults;
  defaults.promoted_mcp.url = "https://backend.example.test/mcp";

  EXPECT_EQ(pbr::ResolvePromotedMcp(defaults, defaults).url, "https://backend.example.test/mcp");

  pbr::AppConfig empty_promoted;
  EXPECT_EQ(pbr::ResolvePromotedMcp(empty_promoted, defaults).url, "https://backend.example.test/mcp");
}

TEST(ConfigJsonTest, RoundTripsMeshRoleFields) {
  pbr::AppConfig config;
  config.mesh.node_enabled = false;
  config.mesh.bootstrap_peers = {
      "/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  config.mesh.prefer_contacts_for_routing = false;
  config.mesh.direct_connections = pp::mesh::DirectAudience::Friendly;
  config.mesh.trusted_relays_only = true;
  config.mesh.reachable_without_relay = true;
  config.mesh.mesh_enabled = false;
  config.mesh.amp_udp_port = 18518;
  config.mesh.capabilities.circuit_relay = true;
  config.mesh.capabilities.media_relay = false;
  config.mesh.capabilities.dht = true;
  config.mesh.capabilities.ledger_gateway = true;
  config.mesh.dht.record_ttl_seconds = 7200;
  config.mesh.dht.find_peer_timeout_ms = 8000;
  config.mesh.pricing.media_relay.mode = "volunteer";
  config.mesh.media_relay_budget.default_per_user_up_bps = 12345;
  config.mesh.media_relay_video.serve_levels = {2};
  config.mesh.media_relay_video.carry_levels = 1;
  config.mesh.media_relay_video.strict = true;

  const pbr::Object out = pbr::AppConfigToObject(config);
  ASSERT_TRUE(out.contains("mesh"));
  const pbr::Object* mesh = out.getObject("mesh");
  ASSERT_NE(mesh, nullptr);
  EXPECT_EQ(mesh->getIf<bool>("node_enabled"), false);
  EXPECT_FALSE(mesh->contains("listen_multiaddr"));
  EXPECT_FALSE(mesh->contains("enable_amp_stack"));
  EXPECT_FALSE(mesh->contains("max_connections"));
  ASSERT_NE(mesh->getArray("bootstrap_peers"), nullptr);
  EXPECT_EQ(mesh->getArray("bootstrap_peers")->elements.size(), 1u);
  EXPECT_EQ(mesh->getIf<bool>("prefer_contacts_for_routing"), false);
  EXPECT_EQ(mesh->getString("direct_connections"), "friendly");
  EXPECT_EQ(mesh->getIf<bool>("mesh_enabled"), false);
  EXPECT_EQ(mesh->getNonNegInt("amp_udp_port"), 18518);
  const pbr::Object* caps = mesh->getObject("capabilities");
  ASSERT_NE(caps, nullptr);
  EXPECT_EQ(caps->getIf<bool>("circuit_relay"), true);
  EXPECT_EQ(caps->getIf<bool>("media_relay"), false);
  EXPECT_EQ(caps->getIf<bool>("dht"), true);
  EXPECT_EQ(caps->getIf<bool>("ledger_gateway"), true);
  const pbr::Object* dht = mesh->getObject("dht");
  ASSERT_NE(dht, nullptr);
  EXPECT_EQ(dht->getNonNegInt("record_ttl_seconds"), 7200);

  pbr::AppConfig parsed;
  pbr::AppConfigFromObject(out, parsed);
  EXPECT_FALSE(parsed.mesh.node_enabled);
  ASSERT_EQ(parsed.mesh.bootstrap_peers.size(), 1u);
  EXPECT_EQ(parsed.mesh.bootstrap_peers[0], config.mesh.bootstrap_peers[0]);
  EXPECT_FALSE(parsed.mesh.prefer_contacts_for_routing);
  EXPECT_EQ(parsed.mesh.direct_connections, pp::mesh::DirectAudience::Friendly);
  EXPECT_TRUE(parsed.mesh.trusted_relays_only);
  EXPECT_TRUE(parsed.mesh.reachable_without_relay);
  EXPECT_FALSE(parsed.mesh.mesh_enabled);
  EXPECT_EQ(parsed.mesh.amp_udp_port, 18518);
  EXPECT_TRUE(parsed.mesh.capabilities.circuit_relay);
  EXPECT_FALSE(parsed.mesh.capabilities.media_relay);
  EXPECT_TRUE(parsed.mesh.capabilities.dht);
  EXPECT_TRUE(parsed.mesh.capabilities.ledger_gateway);
  EXPECT_EQ(parsed.mesh.dht.record_ttl_seconds, 7200);
  EXPECT_EQ(parsed.mesh.dht.find_peer_timeout_ms, 8000);
  EXPECT_EQ(parsed.mesh.media_relay_budget.default_per_user_up_bps, 12345);
  EXPECT_EQ(parsed.mesh.media_relay_video.serve_levels, std::vector<int>{2});
  EXPECT_EQ(parsed.mesh.media_relay_video.carry_levels, 1);
  EXPECT_TRUE(parsed.mesh.media_relay_video.strict);
}

TEST(ConfigJsonTest, RoundTripsDirectoryProviders) {
  pbr::AppConfig config;
  config.directory.base_url = "https://primary.example/api/relay";
  config.directory.transport = "http";
  config.directory.providers = {{"https://a.example/api/relay", "http"},
                                {"https://b.example/api/relay", "http"}};

  const pbr::Object out = pbr::AppConfigToObject(config);
  const pbr::Object* directory = out.getObject("directory");
  ASSERT_NE(directory, nullptr);
  ASSERT_NE(directory->getArray("providers"), nullptr);
  EXPECT_EQ(directory->getArray("providers")->elements.size(), 2u);

  pbr::AppConfig parsed;
  pbr::AppConfigFromObject(out, parsed);
  ASSERT_EQ(parsed.directory.providers.size(), 2u);
  EXPECT_EQ(parsed.directory.providers[0].base_url, "https://a.example/api/relay");
  EXPECT_EQ(parsed.directory.base_url, "https://primary.example/api/relay");

  pbr::DirectoryConfig providers_only;
  providers_only.providers = {{"https://only.example", "http"}};
  pbr::NormalizeDirectoryConfig(providers_only);
  EXPECT_EQ(providers_only.base_url, "https://only.example");
  ASSERT_EQ(pbr::EffectiveDirectoryProviders(providers_only).size(), 1u);
}

TEST(ConfigJsonTest, ProfilePrefsSidebarFieldsDefaultWhenAbsentAndRoundTrip) {
  // Preferences written before the sidebar fields existed must still load with defaults.
  auto legacy = pbr::TryParseObject(R"({ "schema_version": 14, "appearance": "dark" })");
  ASSERT_TRUE(legacy.has_value());
  pbr::ProfilePreferences loaded;
  loaded.sidebar_width_dp = 999;
  loaded.sidebar_collapsed = true;
  loaded.auxiliary_width_dp = 999;
  pbr::ProfilePrefsFromObject(*legacy, loaded);
  EXPECT_EQ(loaded.appearance, "dark");
  EXPECT_EQ(loaded.sidebar_width_dp, 240);
  EXPECT_FALSE(loaded.sidebar_collapsed);
  EXPECT_EQ(loaded.auxiliary_width_dp, 320);
  EXPECT_FALSE(loaded.show_advanced_settings) << "technical options stay hidden unless the user opted in";

  pbr::ProfilePreferences edited;
  edited.show_advanced_settings = true;
  edited.sidebar_width_dp = 320;
  edited.sidebar_collapsed = true;
  edited.auxiliary_width_dp = 400;
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_EQ(parsed.sidebar_width_dp, 320);
  EXPECT_TRUE(parsed.sidebar_collapsed);
  EXPECT_EQ(parsed.auxiliary_width_dp, 400);
  EXPECT_TRUE(parsed.show_advanced_settings);
}

TEST(ConfigJsonTest, ProfilePrefsHomeWidgetsHiddenDefaultsEmptyAndRoundTrips) {
  // Edit Home (DESIGN-pp-ui 8.16): the widgets switched off, device-only; absent = every widget shown.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  loaded.home_widgets_hidden = {"stale"};
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_TRUE(loaded.home_widgets_hidden.empty());

  pbr::ProfilePreferences edited;
  edited.home_widgets_hidden = {"brief", "new_people"};
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_EQ(parsed.home_widgets_hidden, (std::vector<std::string>{"brief", "new_people"}));

  // Edit Home's order: absent reads back as none (the default order), a written one comes back as it was.
  EXPECT_TRUE(loaded.home_widgets_order.empty());
  edited.home_widgets_order = {"brief", "favorites", "calls"};
  pbr::ProfilePreferences ordered;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), ordered);
  EXPECT_EQ(ordered.home_widgets_order, (std::vector<std::string>{"brief", "favorites", "calls"}));
}

TEST(ConfigJsonTest, ProfilePrefsStarredContactsRoundTripInStarOrder) {
  // Starred contacts (常用联系人) are kept in the order they were starred; absent reads back as none, a repeat once.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  loaded.starred_contacts = {"stale"};
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_TRUE(loaded.starred_contacts.empty());

  pbr::ProfilePreferences edited;
  edited.starred_contacts = {"c-2", "c-1"};
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_EQ(parsed.starred_contacts, (std::vector<std::string>{"c-2", "c-1"}));

  auto repeated = pbr::TryParseObject(R"({ "starred_contacts": ["a", "", "b", "a", 3] })");
  ASSERT_TRUE(repeated.has_value());
  pbr::ProfilePreferences cleaned;
  pbr::ProfilePrefsFromObject(*repeated, cleaned);
  EXPECT_EQ(cleaned.starred_contacts, (std::vector<std::string>{"a", "b"}));
}

TEST(ConfigJsonTest, ProfilePrefsAiDotDefaultsRoundTripAndUnknownLook) {
  // Phone AI dot (DESIGN-pp-ui §8.14): on, two eyes, medium, right edge, 600 down its band unless written otherwise.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_TRUE(loaded.ai_dot_shown);
  EXPECT_EQ(loaded.ai_dot_look, "b");
  EXPECT_EQ(loaded.ai_dot_size, "m");
  EXPECT_FALSE(loaded.ai_dot_left);
  EXPECT_EQ(loaded.ai_dot_y_permille, 600);

  pbr::ProfilePreferences edited;
  edited.ai_dot_shown = false;
  edited.ai_dot_look = "c";
  edited.ai_dot_size = "l";
  edited.ai_dot_left = true;
  edited.ai_dot_y_permille = 125;
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_FALSE(parsed.ai_dot_shown);
  EXPECT_EQ(parsed.ai_dot_look, "c");
  EXPECT_EQ(parsed.ai_dot_size, "l");
  EXPECT_TRUE(parsed.ai_dot_left);
  EXPECT_EQ(parsed.ai_dot_y_permille, 125);

  auto odd = pbr::TryParseObject(R"({ "ai_dot_look": "z", "ai_dot_size": "xl", "ai_dot_y_permille": 5000 })");
  ASSERT_TRUE(odd.has_value());
  pbr::ProfilePreferences clamped;
  pbr::ProfilePrefsFromObject(*odd, clamped);
  EXPECT_EQ(clamped.ai_dot_look, "b");
  EXPECT_EQ(clamped.ai_dot_size, "m");
  EXPECT_EQ(clamped.ai_dot_y_permille, 1000);
}

TEST(ConfigJsonTest, AttachmentAutoNetworkFlagsDefaultRoundTripAndNormalize) {
  auto legacy = pbr::TryParseObject(R"({ "schema_version": 14, "attachment_download_policy": "always_auto" })");
  ASSERT_TRUE(legacy.has_value());
  pbr::ProfilePreferences loaded;
  loaded.attachment_auto_on_wifi = false;
  loaded.attachment_auto_on_cellular = true;
  pbr::ProfilePrefsFromObject(*legacy, loaded);
  EXPECT_EQ(loaded.attachment_download_policy, "always_auto");
  EXPECT_TRUE(loaded.attachment_auto_on_wifi);
  EXPECT_FALSE(loaded.attachment_auto_on_cellular);

  pbr::ProfilePreferences edited;
  edited.attachment_download_policy = "always_auto";
  edited.attachment_auto_on_wifi = false;
  edited.attachment_auto_on_cellular = true;
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_EQ(parsed.attachment_download_policy, "always_auto");
  EXPECT_FALSE(parsed.attachment_auto_on_wifi);
  EXPECT_TRUE(parsed.attachment_auto_on_cellular);

  edited.attachment_auto_on_cellular = false;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_EQ(parsed.attachment_download_policy, "smart") << "all automatic with no network ticked is the default";
}

TEST(ConfigJsonTest, NotificationPreviewDefaultsToShownWhenAbsentAndRoundTrips) {
  auto legacy = pbr::TryParseObject(R"({ "schema_version": 14, "show_notifications": true })");
  ASSERT_TRUE(legacy.has_value());
  pbr::ProfilePreferences loaded;
  loaded.show_notification_preview = false;
  pbr::ProfilePrefsFromObject(*legacy, loaded);
  EXPECT_TRUE(loaded.show_notification_preview);

  pbr::ProfilePreferences hidden;
  hidden.show_notification_preview = false;
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(hidden), parsed);
  EXPECT_FALSE(parsed.show_notification_preview);
}

namespace {

// Answers every lookup, so the test sees which names the config layer is willing to ask for.
class RecordingCredentialStore final : public pbr::ICredentialStore {
public:
  std::string Get(const std::string& key) const override {
    asked.push_back(key);
    return "value-of-" + key;
  }
  mutable std::vector<std::string> asked;
};

} // namespace

// PB-035: `api_key_env` names an API-key variable, nothing else. A config file must not be able to read an
// arbitrary environment variable (a cloud secret, a session token) and send it as the bearer key.
TEST(ConfigJsonTest, ApiKeyEnvOnlyReadsApiKeyVariables) {
  RecordingCredentialStore store;
  pbr::ICredentialStore::SetInstance(&store);

  EXPECT_TRUE(pbr::IsApiKeyEnvName("OPENAI_API_KEY"));
  EXPECT_TRUE(pbr::IsApiKeyEnvName("BRAVE_SEARCH_API_KEY"));
  EXPECT_FALSE(pbr::IsApiKeyEnvName("AWS_SECRET_ACCESS_KEY"));
  EXPECT_FALSE(pbr::IsApiKeyEnvName("HOME"));
  EXPECT_FALSE(pbr::IsApiKeyEnvName("_API_KEY"));
  EXPECT_FALSE(pbr::IsApiKeyEnvName("openai_api_key"));
  EXPECT_FALSE(pbr::IsApiKeyEnvName("X_API_KEY "));

  auto bad = pbr::TryParseObject(R"({
    "llm": { "api_key_env": "AWS_SECRET_ACCESS_KEY" }
  })");
  ASSERT_TRUE(bad.has_value());
  pbr::AppConfig config;
  pbr::AppConfigFromObject(*bad, config);
  pbr::ResolveConfigCredentials(config);
  EXPECT_TRUE(config.llm.api_key.empty());
  EXPECT_TRUE(store.asked.empty()) << "a rejected name must not even be looked up";

  auto good = pbr::TryParseObject(R"({
    "llm": { "api_key_env": "OPENAI_API_KEY" }
  })");
  ASSERT_TRUE(good.has_value());
  pbr::AppConfig ok;
  pbr::AppConfigFromObject(*good, ok);
  pbr::ResolveConfigCredentials(ok);
  EXPECT_EQ(ok.llm.api_key, "value-of-OPENAI_API_KEY");

  pbr::ICredentialStore::SetInstance(nullptr);
}

// PB-037: a key that came from an environment variable is never written back to config.json. Saving keeps
// the `api_key_env` reference and omits the resolved value.
TEST(ConfigJsonTest, SavingKeepsEnvReferencesAndNeverWritesResolvedKeys) {
  RecordingCredentialStore store;
  pbr::ICredentialStore::SetInstance(&store);

  auto parsed = pbr::TryParseObject(R"({
    "llm": { "api_key_env": "OPENAI_API_KEY" }
  })");
  ASSERT_TRUE(parsed.has_value());
  pbr::AppConfig config;
  pbr::AppConfigFromObject(*parsed, config);
  pbr::ResolveConfigCredentials(config);
  ASSERT_EQ(config.llm.api_key, "value-of-OPENAI_API_KEY");

  const std::string saved = pbr::DumpJson(pbr::ConfigToObject(config, 1));
  EXPECT_EQ(saved.find("value-of-"), std::string::npos) << saved;
  EXPECT_NE(saved.find("OPENAI_API_KEY"), std::string::npos) << saved;

  // A key the user typed never reaches config.json either: it belongs to the profile's secrets.enc.
  pbr::AppConfig typed;
  typed.llm.api_key = "sk-typed";
  typed.llm_api_key_env.clear();
  const std::string typed_saved = pbr::DumpJson(pbr::ConfigToObject(typed, 1));
  EXPECT_EQ(typed_saved.find("sk-typed"), std::string::npos) << typed_saved;
  EXPECT_EQ(typed_saved.find("\"api_key\""), std::string::npos) << typed_saved;

  pbr::ICredentialStore::SetInstance(nullptr);
}
