// A backend's table can lack a service (a custom one that names none). The feature stays in the settings model, off, with
// a plain-language reason — it does not disappear and nothing else's address stands in (profile-model P2).
#include "feature/settings/AboutSettingsSection.h"
#include "feature/settings/BackendServiceEntries.h"
#include "feature/settings/IntegrationsSettingsSection.h"
#include "feature/settings/LlmSettingsSection.h"
#include "feature/settings/NetworkSettingsSection.h"

#include "foundation/data/Config.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

void LoadLocale(const char* language) {
#ifdef PP_BROWSER_ASSETS_DIR
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
  pbr::LocalizationService::Instance().SetPreferredLanguage(language);
}

pbr::CustomServices FullCustom() {
  pbr::CustomServices services;
  services.org_relay_url = "https://custom.example.test/api/relay";
  services.org_relay_id = "relay:custom";
  services.registration_url = "https://custom.example.test/api/relay";
  services.directory_providers = {"https://custom.example.test/api/relay"};
  services.account_locker_url = "https://custom.example.test/api/relay";
  services.client_compat_url = "https://custom.example.test/api/relay";
  services.llm_gateway_url = "https://custom.example.test/api/llm/v1";
  services.llm_token_url = "https://custom.example.test/api/llm/v1";
  services.promoted_mcp_url = "https://custom.example.test/mcp";
  services.crash_intake_url = "https://custom.example.test/api/relay";
  return services;
}

struct Case {
  const char* entry;
  void (*remove)(pbr::CustomServices&);
};

const Case kCases[] = {
    {"ai", [](pbr::CustomServices& s) { s.llm_gateway_url.clear(); }},
    {"ai_key", [](pbr::CustomServices& s) { s.llm_token_url.clear(); }},
    {"people", [](pbr::CustomServices& s) { s.directory_providers.clear(); }},
    {"feeds", [](pbr::CustomServices& s) { s.promoted_mcp_url.clear(); }},
    {"crash_reports", [](pbr::CustomServices& s) { s.crash_intake_url.clear(); }},
    {"account_backup", [](pbr::CustomServices& s) { s.account_locker_url.clear(); }},
    {"support_info", [](pbr::CustomServices& s) { s.client_compat_url.clear(); }},
};

const pbr::BackendServiceEntry* Find(const std::vector<pbr::BackendServiceEntry>& entries, const std::string& id) {
  const auto it = std::find_if(entries.begin(), entries.end(), [&id](const auto& e) { return e.id == id; });
  return it == entries.end() ? nullptr : &*it;
}

} // namespace

// For each service in turn removed from a custom table: the entry is still in the model, off, with its reason; the
// other six are on and say nothing.
TEST(BackendServiceEntriesTest, ARemovedServiceKeepsItsEntryWithAReason) {
  LoadLocale("en");
  for (const Case& test : kCases) {
    SCOPED_TRACE(test.entry);
    pbr::CustomServices services = FullCustom();
    test.remove(services);
    const auto entries = pbr::BackendServiceEntries(pbr::CustomBackendTable(services, pbr::BuildKind::Store), false);
    ASSERT_EQ(entries.size(), 7U) << "no entry disappears";
    for (const pbr::BackendServiceEntry& entry : entries) {
      EXPECT_FALSE(entry.title.empty());
      if (entry.id == test.entry) {
        EXPECT_FALSE(entry.available);
        EXPECT_FALSE(entry.reason.empty());
        EXPECT_EQ(entry.reason.rfind("This custom service has no ", 0), 0U) << entry.reason;
        // Plain words: no protocol terms (memory: plain wording in dialogs).
        for (const char* term : {"MCP", "URL", "relay", "token", "API", "HTTP", "locker", "compat"}) {
          EXPECT_EQ(entry.reason.find(term), std::string::npos) << term << " in: " << entry.reason;
        }
      } else {
        EXPECT_TRUE(entry.available) << entry.id;
        EXPECT_TRUE(entry.reason.empty()) << entry.id;
      }
    }
  }
}

TEST(BackendServiceEntriesTest, TheReasonIsInBothLanguages) {
  pbr::CustomServices services = FullCustom();
  services.llm_gateway_url.clear();
  const pbr::BackendTable table = pbr::CustomBackendTable(services, pbr::BuildKind::Store);
  LoadLocale("en");
  EXPECT_EQ(Find(pbr::BackendServiceEntries(table, false), "ai")->reason, "This custom service has no AI address set.");
  LoadLocale("zh-Hans");
  const std::string zh = Find(pbr::BackendServiceEntries(table, false), "ai")->reason;
  EXPECT_FALSE(zh.empty());
  EXPECT_NE(zh, "This custom service has no AI address set.");
  EXPECT_NE(zh, "settings.service.ai.missing") << "the key exists in zh-Hans.json";
  LoadLocale("en");
}

TEST(BackendServiceEntriesTest, ABuiltInBackendHasEveryServiceAndAUsersOwnFeedNeedsNoneOfTheBackends) {
  LoadLocale("en");
  pbr::CompiledOrigins compiled{.prod = "https://prod.example.test", .prod_relay_id = "relay:prod"};
  for (const pbr::BackendServiceEntry& entry :
       pbr::BackendServiceEntries(pbr::BuiltInBackendTable(pbr::BackendEnv::Prod, compiled), false)) {
    EXPECT_TRUE(entry.available) << entry.id;
  }
  pbr::CustomServices services = FullCustom();
  services.promoted_mcp_url.clear();
  const pbr::BackendTable table = pbr::CustomBackendTable(services, pbr::BuildKind::Store);
  EXPECT_FALSE(Find(pbr::BackendServiceEntries(table, false), "feeds")->available);
  EXPECT_TRUE(Find(pbr::BackendServiceEntries(table, true), "feeds")->available);
}

// The settings sections put the entries in the page state the settings pages read, from the table this run latched.
TEST(BackendServiceEntriesTest, TheSettingsSectionsListThemFromTheActiveTable) {
  LoadLocale("en");
  const pbr::ScopedCompiledOriginsForTest compiled(
      pbr::CompiledOrigins{.prod = "https://prod.example.test", .prod_relay_id = "relay:prod"});
  pbr::CustomServices services = FullCustom();
  services.llm_gateway_url.clear();
  services.crash_intake_url.clear();
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom, services);
  pbr::BootstrapResult bootstrap;
  bootstrap.config = pbr::Config::DefaultAppConfig(false);
  bootstrap.config.backend.env = "custom";
  pbr::SettingsUiState state;
  pbr::NetworkSettingsSection{}.SyncFromSession(bootstrap, state);
  ASSERT_EQ(state.backend_services.size(), 7U);
  EXPECT_FALSE(Find(state.backend_services, "ai")->available);
  EXPECT_FALSE(Find(state.backend_services, "crash_reports")->available);
  EXPECT_TRUE(Find(state.backend_services, "people")->available);
  for (const char* id : {"ai", "crash_reports"}) {
    EXPECT_FALSE(Find(state.backend_services, id)->reason.empty());
  }
  // The same list on the pages that hold the AI and feed entries.
  pbr::SettingsUiState llm_state;
  pbr::LlmSettingsSection{}.SyncFromSession(bootstrap, llm_state);
  EXPECT_FALSE(Find(llm_state.backend_services, "ai")->available);
  pbr::SettingsUiState integrations_state;
  pbr::IntegrationsSettingsSection{}.SyncFromSession(bootstrap, integrations_state);
  ASSERT_EQ(integrations_state.backend_services.size(), 7U);
  EXPECT_TRUE(Find(integrations_state.backend_services, "feeds")->available);
}
