// Parity of the built-in backends: what prod / staging / dev resolve to for a build's compiled origins, recorded from
// the behaviour before the service tables existed (one origin, fixed paths). A built-in table must equal these
// literals; a change here is a change to what existing installs talk to (profile-model P2, DEV/PLAN D).
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

constexpr pbr::CompiledOrigins kStoreAll{.prod = "https://prod.example.test",
                                         .staging = "https://staging.example.test",
                                         .dev = "https://dev.example.test",
                                         .prod_relay_id = "relay:prod",
                                         .staging_relay_id = "relay:staging",
                                         .dev_relay_id = "relay:dev",
                                         .prod_relay_certificate = "cHJvZA==",
                                         .staging_relay_certificate = "c3RhZ2luZw==",
                                         .dev_relay_certificate = "ZGV2"};
constexpr pbr::CompiledOrigins kStoreProdOnly{
    .prod = "https://prod.example.test/", .prod_relay_id = "pending", .prod_relay_certificate = "cHJvZA=="};
constexpr pbr::CompiledOrigins kTesterAll{.staging = "https://staging.example.test",
                                          .dev = "https://dev.example.test",
                                          .staging_relay_id = "relay:staging",
                                          .dev_relay_id = "relay:dev",
                                          .staging_relay_certificate = "c3RhZ2luZw==",
                                          .dev_relay_certificate = "ZGV2",
                                          .kind = pbr::BuildKind::Tester};

/** What one backend resolved to before the tables: the origin, the three derived URLs and the org relay identity. */
struct Golden {
  const char* origin;
  const char* llm;
  const char* relay;
  const char* mcp;
  const char* relay_id;
  const char* certificate;
};

struct Row {
  const char* name;
  pbr::BackendEnv env;
  const pbr::CompiledOrigins* compiled;
  Golden golden;
};

const Row kRows[] = {
    {"store prod",
     pbr::BackendEnv::Prod,
     &kStoreAll,
     {"https://prod.example.test", "https://prod.example.test/api/llm/v1", "https://prod.example.test/api/relay",
      "https://prod.example.test/mcp", "relay:prod", "cHJvZA=="}},
    {"store staging",
     pbr::BackendEnv::Staging,
     &kStoreAll,
     {"https://staging.example.test", "https://staging.example.test/api/llm/v1",
      "https://staging.example.test/api/relay", "https://staging.example.test/mcp", "relay:staging", "c3RhZ2luZw=="}},
    {"store dev",
     pbr::BackendEnv::Dev,
     &kStoreAll,
     {"https://dev.example.test", "https://dev.example.test/api/llm/v1", "https://dev.example.test/api/relay",
      "https://dev.example.test/mcp", "relay:dev", "ZGV2"}},
    {"store prod only, trailing slash, pending relay id",
     pbr::BackendEnv::Prod,
     &kStoreProdOnly,
     {"https://prod.example.test", "https://prod.example.test/api/llm/v1", "https://prod.example.test/api/relay",
      "https://prod.example.test/mcp", "pending", "cHJvZA=="}},
    // Recorded, not assumed: with no staging origin compiled there is nothing to resolve (the latch runs prod instead,
    // ResolveLatchedBackend, so a table is never asked for it).
    {"store staging without a compiled origin", pbr::BackendEnv::Staging, &kStoreProdOnly, {"", "", "", "", "", ""}},
    {"tester staging",
     pbr::BackendEnv::Staging,
     &kTesterAll,
     {"https://staging.example.test", "https://staging.example.test/api/llm/v1",
      "https://staging.example.test/api/relay", "https://staging.example.test/mcp", "relay:staging", "c3RhZ2luZw=="}},
    {"tester dev",
     pbr::BackendEnv::Dev,
     &kTesterAll,
     {"https://dev.example.test", "https://dev.example.test/api/llm/v1", "https://dev.example.test/api/relay",
      "https://dev.example.test/mcp", "relay:dev", "ZGV2"}},
};

} // namespace

// The recording itself: the rule before the tables, run on the fixtures, equals the literals above.
TEST(BackendTableGoldenTest, TheOneOriginRuleStillGivesTheRecordedUrls) {
  for (const Row& row : kRows) {
    SCOPED_TRACE(row.name);
    const std::string origin = pbr::ResolveBriefOrigin(false, nullptr, row.env, *row.compiled);
    EXPECT_EQ(origin, row.golden.origin);
    EXPECT_EQ(pbr::BriefLlmBaseUrlFor(origin), row.golden.llm);
    EXPECT_EQ(pbr::BriefRelayBaseUrlFor(origin), row.golden.relay);
    EXPECT_EQ(pbr::BriefMcpUrlFor(origin), row.golden.mcp);
    const pbr::OrgRelayIdentity identity = pbr::ResolveOrgRelayIdentity(false, nullptr, row.env, *row.compiled);
    EXPECT_EQ(identity.relay_id, row.golden.relay_id);
    EXPECT_EQ(identity.certificate_b64, row.golden.certificate);
  }
}

// The built-in tables are today's resolved URLs, field by field (the services of one operator sit under its origin).
TEST(BackendTableGoldenTest, EveryBuiltInTableEqualsTheRecordedUrls) {
  for (const Row& row : kRows) {
    SCOPED_TRACE(row.name);
    const pbr::BackendTable table = pbr::BuiltInBackendTable(row.env, *row.compiled);
    const std::string relay = row.golden.relay;
    EXPECT_EQ(table.org_relay_url, relay);
    EXPECT_EQ(table.registration_url, relay);
    EXPECT_EQ(table.directory_providers, relay.empty() ? std::vector<std::string>{} : std::vector<std::string>{relay});
    EXPECT_EQ(table.account_locker_url, relay) << "lockers are where the registration URL is";
    EXPECT_EQ(table.client_compat_url, relay);
    EXPECT_EQ(table.crash_intake_url, relay);
    EXPECT_EQ(table.mesh_nodes_url, relay);
    EXPECT_EQ(table.llm_gateway_url, row.golden.llm);
    EXPECT_EQ(table.llm_token_url, row.golden.llm) << "the renew path is appended: <origin>/api/llm/v1/token/renew";
    EXPECT_EQ(table.promoted_mcp_url, row.golden.mcp);
    EXPECT_EQ(table.org_relay_id, row.golden.relay_id);
    EXPECT_EQ(table.registration_relay_id, row.golden.relay_id);
    EXPECT_EQ(table.org_relay_certificate_b64, row.golden.certificate);
    EXPECT_TRUE(table.seed_relays.empty());
  }
}

TEST(BackendTableGoldenTest, BuiltInIdsAndNetworks) {
  EXPECT_EQ(pbr::BuiltInBackendTable(pbr::BackendEnv::Prod, kStoreAll).id, "prod");
  EXPECT_EQ(pbr::BuiltInBackendTable(pbr::BackendEnv::Prod, kStoreAll).network, pbr::BackendNetwork::Main);
  EXPECT_EQ(pbr::BuiltInBackendTable(pbr::BackendEnv::Staging, kStoreAll).id, "staging");
  EXPECT_EQ(pbr::BuiltInBackendTable(pbr::BackendEnv::Staging, kStoreAll).network, pbr::BackendNetwork::Staging);
  EXPECT_EQ(pbr::BuiltInBackendTable(pbr::BackendEnv::Dev, kTesterAll).id, "dev");
  EXPECT_EQ(pbr::BuiltInBackendTable(pbr::BackendEnv::Dev, kTesterAll).network, pbr::BackendNetwork::Dev);
  // A tester build asked for prod runs its first usable environment (never prod), and the table says so.
  EXPECT_EQ(pbr::BuiltInBackendTable(pbr::BackendEnv::Prod, kTesterAll).id, "staging");
  EXPECT_STREQ(pbr::BackendNetworkName(pbr::BackendNetwork::TestCustom), "test-custom");
}

// PP_BROWSER_ORIGIN: every service under the one origin, the relay id as PP_BROWSER_RELAY_ID gives it, no certificate.
TEST(BackendTableGoldenTest, AnOriginTableIsTheOneOriginRule) {
  const pbr::BackendTable table = pbr::ResolveBackendTable(false, "https://override.example.test//", "relay:override",
                                                           pbr::BackendEnv::Prod, kStoreAll, {});
  EXPECT_EQ(table.org_relay_url, "https://override.example.test/api/relay");
  EXPECT_EQ(table.llm_gateway_url, "https://override.example.test/api/llm/v1");
  EXPECT_EQ(table.promoted_mcp_url, "https://override.example.test/mcp");
  EXPECT_EQ(table.org_relay_id, "relay:override");
  EXPECT_TRUE(table.org_relay_certificate_b64.empty());
  EXPECT_EQ(table.id, "custom");
  EXPECT_EQ(table.network, pbr::BackendNetwork::Main);
  EXPECT_EQ(
      pbr::ResolveBackendTable(false, "https://override.example.test", nullptr, pbr::BackendEnv::Dev, kTesterAll, {})
          .network,
      pbr::BackendNetwork::TestCustom);
  EXPECT_TRUE(
      pbr::ResolveBackendTable(true, "https://override.example.test", nullptr, pbr::BackendEnv::Prod, kStoreAll, {})
          .org_relay_url.empty())
      << "the UI demo app has no backend";
}

namespace {

pbr::CustomServices FullCustom() {
  pbr::CustomServices services;
  services.org_relay_url = "https://custom.example.test/api/relay";
  services.org_relay_id = "relay:custom";
  services.registration_url = "https://custom.example.test/api/relay";
  services.registration_relay_id = "relay:custom";
  services.directory_providers = {"https://custom.example.test/api/relay", "https://custom-two.example.test/api/relay"};
  services.account_locker_url = "https://custom.example.test/api/relay";
  services.client_compat_url = "https://custom.example.test/api/relay";
  services.llm_gateway_url = "https://custom.example.test/api/llm/v1";
  services.llm_token_url = "https://custom.example.test/api/llm/v1";
  services.promoted_mcp_url = "https://custom.example.test/mcp";
  services.crash_intake_url = "https://custom.example.test/api/relay";
  services.mesh_nodes_url = "https://custom.example.test/api/relay";
  services.seed_relays = {"https://custom-extra.example.test/api/relay"};
  return services;
}

/** Every address a table holds. */
std::vector<std::string> AddressesOf(const pbr::BackendTable& table) {
  std::vector<std::string> out = {table.org_relay_url,     table.registration_url, table.account_locker_url,
                                  table.client_compat_url, table.llm_gateway_url,  table.llm_token_url,
                                  table.promoted_mcp_url,  table.crash_intake_url, table.mesh_nodes_url};
  out.insert(out.end(), table.directory_providers.begin(), table.directory_providers.end());
  out.insert(out.end(), table.seed_relays.begin(), table.seed_relays.end());
  return out;
}

bool NamesAnotherBackend(const std::string& address) {
  return address.find("prod.example.test") != std::string::npos ||
         address.find("staging.example.test") != std::string::npos ||
         address.find("dev.example.test") != std::string::npos;
}

} // namespace

// A service the custom table lacks is absent — whichever one it is — and nothing takes a built-in backend's address.
TEST(BackendTableCustomTest, AServiceRemovedFromACustomTableIsAbsentAndNothingStandsInForIt) {
  struct Removal {
    const char* name;
    void (*remove)(pbr::CustomServices&);
    const std::string& (*field)(const pbr::BackendTable&);
  };
  const Removal removals[] = {
      {"llm gateway", [](pbr::CustomServices& s) { s.llm_gateway_url.clear(); },
       [](const pbr::BackendTable& t) -> const std::string& { return t.llm_gateway_url; }},
      {"llm token", [](pbr::CustomServices& s) { s.llm_token_url.clear(); },
       [](const pbr::BackendTable& t) -> const std::string& { return t.llm_token_url; }},
      {"promoted mcp", [](pbr::CustomServices& s) { s.promoted_mcp_url.clear(); },
       [](const pbr::BackendTable& t) -> const std::string& { return t.promoted_mcp_url; }},
      {"crash intake", [](pbr::CustomServices& s) { s.crash_intake_url.clear(); },
       [](const pbr::BackendTable& t) -> const std::string& { return t.crash_intake_url; }},
      {"account locker", [](pbr::CustomServices& s) { s.account_locker_url.clear(); },
       [](const pbr::BackendTable& t) -> const std::string& { return t.account_locker_url; }},
      {"client compat", [](pbr::CustomServices& s) { s.client_compat_url.clear(); },
       [](const pbr::BackendTable& t) -> const std::string& { return t.client_compat_url; }},
      {"mesh nodes", [](pbr::CustomServices& s) { s.mesh_nodes_url.clear(); },
       [](const pbr::BackendTable& t) -> const std::string& { return t.mesh_nodes_url; }},
  };
  for (const pbr::CompiledOrigins* compiled : {&kStoreAll, &kTesterAll}) {
    for (const Removal& removal : removals) {
      SCOPED_TRACE(removal.name);
      pbr::CustomServices services = FullCustom();
      removal.remove(services);
      const pbr::BackendTable table =
          pbr::ResolveBackendTable(false, nullptr, nullptr, pbr::BackendEnv::Custom, *compiled, services);
      EXPECT_TRUE(removal.field(table).empty());
      for (const std::string& address : AddressesOf(table)) {
        EXPECT_FALSE(NamesAnotherBackend(address)) << address;
      }
      EXPECT_EQ(table.org_relay_id, "relay:custom") << "the rest of the table is unchanged";
    }
    // The directory too: removed, the table has none (not the org relay's, not a built-in's).
    pbr::CustomServices services = FullCustom();
    services.directory_providers.clear();
    const pbr::BackendTable table =
        pbr::ResolveBackendTable(false, nullptr, nullptr, pbr::BackendEnv::Custom, *compiled, services);
    EXPECT_TRUE(table.directory_providers.empty());
    EXPECT_TRUE(table.DirectoryUrl().empty());
  }
}

// #742: a relay or registration without a relay id names none, whatever relay id the build compiled.
TEST(BackendTableCustomTest, ACustomRelayOrRegistrationWithoutARelayIdNamesNone) {
  pbr::CustomServices services = FullCustom();
  services.org_relay_id.clear();
  services.registration_relay_id.clear();
  for (const pbr::CompiledOrigins* compiled : {&kStoreAll, &kTesterAll}) {
    const pbr::BackendTable table =
        pbr::ResolveBackendTable(false, nullptr, nullptr, pbr::BackendEnv::Custom, *compiled, services);
    EXPECT_TRUE(table.org_relay_id.empty());
    EXPECT_TRUE(table.registration_relay_id.empty());
    EXPECT_TRUE(table.org_relay_certificate_b64.empty());
    EXPECT_FALSE(table.org_relay_url.empty());
  }
  // The org relay and the registration name theirs separately.
  services.org_relay_id = "relay:org-only";
  const pbr::BackendTable table = pbr::CustomBackendTable(services, pbr::BuildKind::Store);
  EXPECT_EQ(table.org_relay_id, "relay:org-only");
  EXPECT_TRUE(table.registration_relay_id.empty());
}

TEST(BackendTableCustomTest, ACustomTableIsOfTheBuildsCustomNetwork) {
  EXPECT_EQ(pbr::CustomBackendTable({}, pbr::BuildKind::Store).network, pbr::BackendNetwork::Main);
  EXPECT_EQ(pbr::CustomBackendTable({}, pbr::BuildKind::Store).id, "custom");
  EXPECT_EQ(pbr::CustomBackendTable({}, pbr::BuildKind::Tester).network, pbr::BackendNetwork::TestCustom);
  EXPECT_EQ(pbr::CustomBackendTable({}, pbr::BuildKind::Tester).id, "test-custom");
  EXPECT_TRUE(pbr::CustomServices{}.Empty());
  EXPECT_FALSE(FullCustom().Empty());
  for (const std::string& address : AddressesOf(pbr::CustomBackendTable({}, pbr::BuildKind::Store))) {
    EXPECT_TRUE(address.empty()) << "a custom table with nothing typed in holds nothing";
  }
}

// The latch carries the custom services with the backend; the first latch wins, as the backend does.
TEST(BackendTableCustomTest, TheActiveTableCarriesTheLatchedCustomServices) {
  const pbr::ScopedCompiledOriginsForTest compiled(kStoreAll);
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom, FullCustom());
  EXPECT_EQ(pbr::ActiveBackend(), pbr::BackendEnv::Custom);
  EXPECT_EQ(pbr::ActiveBackendTable().llm_gateway_url, "https://custom.example.test/api/llm/v1");
  EXPECT_EQ(pbr::BriefLlmBaseUrl(), "https://custom.example.test/api/llm/v1");
  EXPECT_EQ(pbr::BriefMcpUrl(), "https://custom.example.test/mcp");
  EXPECT_EQ(pbr::BriefRelayId(), "relay:custom");
  EXPECT_TRUE(pbr::BriefOrigin().empty()) << "a custom backend has no origin of its own";
  EXPECT_TRUE(pbr::DefaultBackendTable().llm_gateway_url.empty()) << "what a config starts from names nothing";
  pbr::CustomServices other;
  other.llm_gateway_url = "https://later.example.test/api/llm/v1";
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom, other);
  EXPECT_EQ(pbr::BriefLlmBaseUrl(), "https://custom.example.test/api/llm/v1") << "the first latch wins";
}
