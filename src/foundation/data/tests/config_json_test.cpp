#include "foundation/crypto/CryptoConstants.h"
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
  config.mesh.capacity.links = 20000;
  config.mesh.capacity.group_calls = 64;

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
  EXPECT_EQ(parsed.mesh.capacity.links, 20000u);
  EXPECT_EQ(parsed.mesh.capacity.group_calls, 64u);
  EXPECT_EQ(parsed.mesh.capacity.reservations, 0u) << "unset keeps the default";
}

TEST(ConfigJsonTest, RoundTripsDirectoryProviders) {
  pbr::AppConfig config;
  config.directory.base_url = "https://primary.example/api/relay";
  config.directory.transport = "http";
  config.directory.providers = {{"https://a.example/api/relay", "http"}, {"https://b.example/api/relay", "http"}};

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

TEST(ConfigJsonTest, ProfilePrefsInvitePoliciesDefaultToEveryoneAndResetOnceBelowSchema16) {
  pbr::ProfilePreferences fresh;
  EXPECT_EQ(fresh.call_invite_policy, "everyone");
  EXPECT_EQ(fresh.group_invite_policy, "everyone");
  EXPECT_EQ(pbr::UserPreferences::DefaultProfile().call_invite_policy, "everyone");
  EXPECT_EQ(pbr::UserPreferences::DefaultProfile().group_invite_policy, "everyone");

  auto parse = [](const char* json) {
    auto object = pbr::TryParseObject(json);
    EXPECT_TRUE(object.has_value());
    pbr::ProfilePreferences prefs;
    pbr::ProfilePrefsFromObject(*object, prefs);
    return prefs;
  };
  // Below schema 16 both values are read as everyone, whatever the file holds.
  for (const char* stored : {"contacts_only", "nobody"}) {
    for (const int version : {14, 15}) {
      const std::string json = "{ \"schema_version\": " + std::to_string(version) + ", \"call_invite_policy\": \"" +
                               stored + "\", \"group_invite_policy\": \"" + stored + "\" }";
      const auto prefs = parse(json.c_str());
      EXPECT_EQ(prefs.call_invite_policy, "everyone") << json;
      EXPECT_EQ(prefs.group_invite_policy, "everyone") << json;
      EXPECT_EQ(prefs.schema_version, pbr::ProfilePreferences::kSchemaVersion) << "saved back as current: " << json;
    }
  }
  EXPECT_EQ(parse(R"({ "schema_version": 15 })").call_invite_policy, "everyone");
  EXPECT_EQ(parse(R"({ "schema_version": 15 })").group_invite_policy, "everyone");

  // From schema 16 the stored value stands; a missing key reads as everyone.
  const auto kept =
      parse(R"({ "schema_version": 16, "call_invite_policy": "contacts_only", "group_invite_policy": "nobody" })");
  EXPECT_EQ(kept.call_invite_policy, "contacts_only");
  EXPECT_EQ(kept.group_invite_policy, "nobody");
  const auto nobody = parse(R"({ "schema_version": 16, "call_invite_policy": "nobody" })");
  EXPECT_EQ(nobody.call_invite_policy, "nobody");
  EXPECT_EQ(nobody.group_invite_policy, "everyone");
  EXPECT_EQ(parse(R"({ "schema_version": 16 })").call_invite_policy, "everyone");
  EXPECT_EQ(parse(R"({ "schema_version": 16 })").group_invite_policy, "everyone");

  // A saved file survives a round trip unchanged.
  pbr::ProfilePreferences edited;
  edited.call_invite_policy = "contacts_only";
  pbr::ProfilePreferences round;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), round);
  EXPECT_EQ(round.call_invite_policy, "contacts_only");
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

TEST(ConfigJsonTest, ProfilePrefsRecoveryPhraseSeenRoundTripsAndDefaultsOff) {
  // Home's Security widget: absent (a fresh profile) reads as not yet seen; once set it stays set.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  loaded.recovery_phrase_seen = true;
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_FALSE(loaded.recovery_phrase_seen);

  pbr::ProfilePreferences edited;
  edited.recovery_phrase_seen = true;
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_TRUE(parsed.recovery_phrase_seen);
}

TEST(ConfigJsonTest, ProfilePrefsUnlockGraceMinutesRoundTripsAndRejectsOddValues) {
  // PIN grace window (#21): absent reads 15; 0/5/15/60/-1 (always) round-trip; anything else reads back as 15.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  loaded.unlock_grace_minutes = 60;
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_EQ(loaded.unlock_grace_minutes, 15);

  for (int minutes : {0, 5, 15, 60, pbr::kUnlockGraceAlways}) {
    pbr::ProfilePreferences edited;
    edited.unlock_grace_minutes = minutes;
    pbr::ProfilePreferences parsed;
    pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
    EXPECT_EQ(parsed.unlock_grace_minutes, minutes);
  }

  for (const char* odd_json :
       {R"({ "unlock_grace_minutes": 7 })", R"({ "unlock_grace_minutes": -2 })", R"({ "unlock_grace_minutes": -60 })",
        R"({ "unlock_grace_minutes": 100000 })", R"({ "unlock_grace_minutes": "15" })"}) {
    auto odd = pbr::TryParseObject(odd_json);
    ASSERT_TRUE(odd.has_value());
    pbr::ProfilePreferences parsed;
    parsed.unlock_grace_minutes = 0;
    pbr::ProfilePrefsFromObject(*odd, parsed);
    EXPECT_EQ(parsed.unlock_grace_minutes, 15) << odd_json;
  }
}

TEST(ConfigJsonTest, ProfilePrefsPinDigitsUnknownWhenAbsentAndRejectsOddValues) {
  // DEV/PLAN-pin-gate-redesign.md B: absent = unknown (no schema bump); 0 (a text PIN) and 4..8 round-trip; anything
  // else reads back as unknown.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  loaded.pin_digits = 6;
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_EQ(loaded.pin_digits, std::nullopt);
  EXPECT_FALSE(pbr::ProfilePrefsToObject(loaded).getIf<int64_t>("pin_digits").has_value()) << "unknown is not written";

  for (int digits : {0, 6}) {
    pbr::ProfilePreferences edited;
    edited.pin_digits = digits;
    pbr::ProfilePreferences parsed;
    pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
    EXPECT_EQ(parsed.pin_digits, digits);
  }

  for (const char* odd_json :
       {R"({ "pin_digits": 3 })", R"({ "pin_digits": 9 })", R"({ "pin_digits": -1 })", R"({ "pin_digits": true })"}) {
    auto odd = pbr::TryParseObject(odd_json);
    ASSERT_TRUE(odd.has_value());
    pbr::ProfilePreferences parsed;
    parsed.pin_digits = 6;
    pbr::ProfilePrefsFromObject(*odd, parsed);
    EXPECT_EQ(parsed.pin_digits, std::nullopt) << odd_json;
  }
}

TEST(ConfigJsonTest, ProfilePrefsHomeShowDynamicRoundTripsAndDefaultsOff) {
  // Minimal AI-first Home: absent (a fresh or pre-existing profile) reads off; once set it stays set.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  loaded.home_show_dynamic = true;
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_FALSE(loaded.home_show_dynamic);

  pbr::ProfilePreferences edited;
  edited.home_show_dynamic = true;
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_TRUE(parsed.home_show_dynamic);
}

TEST(ConfigJsonTest, ProfilePrefsHomeAnimatedLogoRoundTripsAndDefaultsOn) {
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  loaded.home_animated_logo = false;
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_TRUE(loaded.home_animated_logo);

  pbr::ProfilePreferences edited;
  edited.home_animated_logo = false;
  pbr::ProfilePreferences parsed;
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(edited), parsed);
  EXPECT_FALSE(parsed.home_animated_logo);
}

TEST(ConfigJsonTest, ProfilePrefsAiDotDefaultsRoundTripAndUnknownLook) {
  // Phone AI dot (DESIGN-pp-ui §8.14): on, two eyes, medium, right edge, 600 down its band unless written otherwise.
  auto absent = pbr::TryParseObject(R"({ "schema_version": 14 })");
  ASSERT_TRUE(absent.has_value());
  pbr::ProfilePreferences loaded;
  pbr::ProfilePrefsFromObject(*absent, loaded);
  EXPECT_TRUE(loaded.ai_dot_shown);
  EXPECT_EQ(loaded.ai_dot_look, "auto") << "a different look each day unless one is picked";
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

  pbr::ProfilePreferences automatic;
  automatic.ai_dot_look = "auto";
  pbr::ProfilePreferences automatic_parsed;
  automatic_parsed.ai_dot_look = "d";
  pbr::ProfilePrefsFromObject(pbr::ProfilePrefsToObject(automatic), automatic_parsed);
  EXPECT_EQ(automatic_parsed.ai_dot_look, "auto") << "auto survives a round trip";

  auto odd = pbr::TryParseObject(R"({ "ai_dot_look": "z", "ai_dot_size": "xl", "ai_dot_y_permille": 5000 })");
  ASSERT_TRUE(odd.has_value());
  pbr::ProfilePreferences clamped;
  pbr::ProfilePrefsFromObject(*odd, clamped);
  EXPECT_EQ(clamped.ai_dot_look, "auto");
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
