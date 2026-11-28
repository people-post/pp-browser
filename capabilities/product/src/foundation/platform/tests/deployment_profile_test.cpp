#include "foundation/platform/DeploymentProfile.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace {

class ScopedOrigin {
public:
  explicit ScopedOrigin(const char* value) {
    if (const char* old = std::getenv("PP_BROWSER_ORIGIN")) {
      had_ = true;
      old_ = old;
    }
    set(value);
  }
  ~ScopedOrigin() { set(had_ ? old_.c_str() : nullptr); }

private:
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("PP_BROWSER_ORIGIN", value ? value : "");
#else
    if (value) {
      setenv("PP_BROWSER_ORIGIN", value, 1);
    } else {
      unsetenv("PP_BROWSER_ORIGIN");
    }
#endif
  }
  bool had_ = false;
  std::string old_;
};

} // namespace

TEST(DeploymentProfileTest, EnvOriginDerivesUrls) {
  ScopedOrigin origin("https://backend.example.test");
  EXPECT_EQ(pbr::BriefOrigin(), "https://backend.example.test");
  EXPECT_EQ(pbr::BriefLlmBaseUrl(), "https://backend.example.test/api/llm/v1");
  EXPECT_EQ(pbr::BriefRelayBaseUrl(), "https://backend.example.test/api/relay");
  EXPECT_EQ(pbr::BriefMcpUrl(), "https://backend.example.test/mcp");
  EXPECT_STREQ(pbr::ProductDirBasename(), pbr::kProductDirName);
}

TEST(DeploymentProfileTest, TrailingSlashIsTrimmed) {
  ScopedOrigin origin("https://backend.example.test//");
  EXPECT_EQ(pbr::BriefOrigin(), "https://backend.example.test");
}

TEST(DeploymentProfileTest, EmptyEnvFallsBackToCompiledDefault) {
  ScopedOrigin origin("");
  pbr::ResetActiveBackendForTest();
  const pbr::CompiledOrigins compiled = pbr::BuildCompiledOrigins();
  // No latch: the build's fallback — prod in a store build, the tester environment in a tester build.
  EXPECT_EQ(pbr::BriefOrigin(), std::string(pbr::CompiledOriginFor(pbr::FallbackBackendIn(compiled), compiled)));
  if (!pbr::IsTesterBuild()) {
    EXPECT_EQ(pbr::BriefOrigin(), std::string(pbr::CompiledDefaultOrigin()));
  }
}

// Compile-time origins as each build variant has them: the decisions take them as inputs, so every variant runs here.
constexpr pbr::CompiledOrigins kNoOrigins{};
constexpr pbr::CompiledOrigins kProdOnly{.prod = "https://prod.example.test/"};
constexpr pbr::CompiledOrigins kProdAndStaging{.prod = "https://prod.example.test",
                                               .staging = "https://staging.example.test"};

TEST(DeploymentProfileTest, NoOriginYieldsEmptyUrls) {
  const std::string origin = pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Prod, kNoOrigins);
  EXPECT_TRUE(origin.empty());
  EXPECT_TRUE(pbr::BriefLlmBaseUrlFor(origin).empty());
  EXPECT_TRUE(pbr::BriefRelayBaseUrlFor(origin).empty());
  EXPECT_TRUE(pbr::BriefMcpUrlFor(origin).empty());
}

TEST(DeploymentProfileTest, ACompiledDefaultOriginIsUsedWithoutTheEnvironment) {
  const std::string origin = pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Prod, kProdOnly);
  EXPECT_EQ(origin, "https://prod.example.test");
  EXPECT_EQ(pbr::BriefLlmBaseUrlFor(origin), "https://prod.example.test/api/llm/v1");
  EXPECT_EQ(pbr::BriefRelayBaseUrlFor(origin), "https://prod.example.test/api/relay");
  EXPECT_EQ(pbr::BriefMcpUrlFor(origin), "https://prod.example.test/mcp");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, "", pbr::BackendEnv::Prod, kProdOnly), "https://prod.example.test")
      << "an empty variable is unset";
}

TEST(DeploymentProfileTest, BackendEnvNamesRoundTripAndUnknownIsProd) {
  for (const auto env :
       {pbr::BackendEnv::Prod, pbr::BackendEnv::Staging, pbr::BackendEnv::Dev, pbr::BackendEnv::Custom}) {
    EXPECT_EQ(pbr::ParseBackendEnv(pbr::BackendEnvName(env)), env);
  }
  EXPECT_EQ(pbr::ParseBackendEnv(""), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ParseBackendEnv("unknown"), pbr::BackendEnv::Prod);
}

TEST(DeploymentProfileTest, PersistedQaLoadsAsStagingAndIsSavedAsStaging) {
  EXPECT_EQ(pbr::ParseBackendEnv("qa"), pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::ParseBackendEnv("staging"), pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::ParseBackendEnv("dev"), pbr::BackendEnv::Dev);
  EXPECT_STREQ(pbr::BackendEnvName(pbr::ParseBackendEnv("qa")), "staging");
}

TEST(DeploymentProfileTest, EachEnvironmentHasItsCompiledOriginAndCustomHasNone) {
  constexpr pbr::CompiledOrigins kAll{.prod = "https://prod.example.test",
                                      .staging = "https://staging.example.test",
                                      .dev = "https://dev.example.test"};
  EXPECT_STREQ(pbr::CompiledOriginFor(pbr::BackendEnv::Prod, kAll), "https://prod.example.test");
  EXPECT_STREQ(pbr::CompiledOriginFor(pbr::BackendEnv::Staging, kAll), "https://staging.example.test");
  EXPECT_STREQ(pbr::CompiledOriginFor(pbr::BackendEnv::Dev, kAll), "https://dev.example.test");
  EXPECT_STREQ(pbr::CompiledOriginFor(pbr::BackendEnv::Custom, kAll), "");
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Prod, kNoOrigins));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Custom, kNoOrigins));
  EXPECT_FALSE(pbr::BackendAvailableIn(pbr::BackendEnv::Staging, kProdOnly));
  EXPECT_FALSE(pbr::BackendAvailableIn(pbr::BackendEnv::Dev, kProdOnly));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Staging, kAll));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Dev, kAll));
}

TEST(DeploymentProfileTest, ActiveBackendLatchesOnFirstCall) {
  pbr::ResetActiveBackendForTest();
  EXPECT_FALSE(pbr::ActiveBackendLatched());
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom);
  pbr::LatchActiveBackend(pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ActiveBackend(), pbr::BackendEnv::Custom);
  pbr::ResetActiveBackendForTest();
}

// A store build compiles prod only (RC015): a saved staging / dev (or a saved qa) runs as prod.
TEST(DeploymentProfileTest, StagingAndDevFallBackToProdWithoutTheirCompiledOrigin) {
  constexpr pbr::CompiledOrigins kStagingOnly{.prod = "https://prod.example.test",
                                              .staging = "https://staging.example.test"};
  constexpr pbr::CompiledOrigins kAll{.prod = "https://prod.example.test",
                                      .staging = "https://staging.example.test",
                                      .dev = "https://dev.example.test"};
  for (const pbr::BackendEnv env : {pbr::BackendEnv::Staging, pbr::BackendEnv::Dev}) {
    EXPECT_EQ(pbr::ResolveLatchedBackend(env, kProdOnly), pbr::BackendEnv::Prod);
    EXPECT_EQ(pbr::ResolveLatchedBackend(env, kNoOrigins), pbr::BackendEnv::Prod);
    EXPECT_EQ(pbr::ResolveLatchedBackend(env, kAll), env);
  }
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::ParseBackendEnv("qa"), kProdOnly), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Staging, kStagingOnly), pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Dev, kStagingOnly), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Custom, kNoOrigins), pbr::BackendEnv::Custom);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, kAll), pbr::BackendEnv::Prod);
}

// This build's own: the latch applies the rule with its compiled origins.
TEST(DeploymentProfileTest, TheLatchUsesThisBuildsCompiledOrigins) {
  pbr::ResetActiveBackendForTest();
  pbr::LatchActiveBackend(pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::ActiveBackend(), pbr::ResolveLatchedBackend(pbr::BackendEnv::Staging, pbr::BuildCompiledOrigins()));
  pbr::ResetActiveBackendForTest();
}

TEST(DeploymentProfileTest, StagingAndDevUseTheirOriginsAndEnvStillWins) {
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Staging, kProdAndStaging),
            "https://staging.example.test");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Dev, {.dev = "https://dev.example.test"}),
            "https://dev.example.test");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Prod, kProdAndStaging),
            "https://prod.example.test");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, "https://backend.example.test", pbr::BackendEnv::Staging, kProdAndStaging),
            "https://backend.example.test");
}

// Multi-relay MR008, SIGNED_FORMATS § 3.5: the org relay's address carries its relay id, shipped with the build beside
// its origin (PP_RELAY_ID_PROD / _STAGING / _DEV), with its certificate at build time as the floor
// (config/relay-certs/<env>.cert); a developer's PP_BROWSER_ORIGIN names its own (PP_BROWSER_RELAY_ID) and no
// certificate (fetched), or none (first use, warned).
TEST(DeploymentProfileTest, TheOrgRelaysIdAndCertificateShipWithItsBackend) {
  const pbr::CompiledOrigins compiled{.prod = "https://prod.example.test",
                                      .staging = "https://staging.example.test",
                                      .dev = "https://dev.example.test",
                                      .prod_relay_id = "relay:prod",
                                      .staging_relay_id = "relay:staging",
                                      .dev_relay_id = "relay:dev",
                                      .prod_relay_certificate = "cHJvZA==",
                                      .staging_relay_certificate = "c3RhZ2luZw==",
                                      .dev_relay_certificate = "ZGV2"};
  const auto prod = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Prod, compiled);
  EXPECT_EQ(prod.relay_id, "relay:prod");
  EXPECT_EQ(prod.certificate_b64, "cHJvZA==");
  const auto staging = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Staging, compiled);
  EXPECT_EQ(staging.relay_id, "relay:staging");
  EXPECT_EQ(staging.certificate_b64, "c3RhZ2luZw==");
  const auto dev = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Dev, compiled);
  EXPECT_EQ(dev.relay_id, "relay:dev");
  EXPECT_EQ(dev.certificate_b64, "ZGV2");
  const auto legacy = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::ParseBackendEnv("qa"), compiled);
  EXPECT_EQ(legacy.relay_id, "relay:staging") << "a saved qa is staging, and staging's relay is its own";
  EXPECT_EQ(pbr::ResolveOrgRelayIdentity(false, "", pbr::BackendEnv::Prod, compiled).relay_id, "relay:prod")
      << "an empty override";
  const auto other = pbr::ResolveOrgRelayIdentity(false, "https://other.example.test", pbr::BackendEnv::Dev, compiled);
  EXPECT_EQ(other.relay_id, "") << "another origin: not the build's relay";
  EXPECT_EQ(other.certificate_b64, "") << "nor its certificate";
  EXPECT_EQ(
      pbr::ResolveOrgRelayIdentity(false, "https://other.example.test", pbr::BackendEnv::Prod, compiled, "relay:other")
          .relay_id,
      "relay:other")
      << "PP_BROWSER_RELAY_ID names the override's relay";
  const auto demo = pbr::ResolveOrgRelayIdentity(true, nullptr, pbr::BackendEnv::Dev, compiled);
  EXPECT_TRUE(demo.relay_id.empty() && demo.certificate_b64.empty()) << "the UI demo app";
}

// An environment whose operator set is not made yet (Leo: staging's and prod's) has the build's default, a placeholder
// that fails closed.
TEST(DeploymentProfileTest, ThePendingPlaceholderIsNoRelayId) {
  EXPECT_TRUE(pbr::IsPendingRelayId(pbr::kPendingRelayId));
  EXPECT_TRUE(pbr::IsPendingRelayId("pending"));
  EXPECT_FALSE(pbr::IsPendingRelayId(""));
  EXPECT_FALSE(pbr::IsPendingRelayId("relay:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8"));
}

// MR008 item 4: first use of the org's relay only in a developer build or on the compiled backend (a dogfood build
// shipping no PeerId); a release build's custom backend or PP_BROWSER_ORIGIN never.
TEST(DeploymentProfileTest, TheOrgRelayIsTakenOnFirstUseOnlyInDeveloperBuildsOrOnTheCompiledBackend) {
  EXPECT_TRUE(pbr::OrgRelayFirstUseAllowed(true, false)) << "a developer build's custom backend";
  EXPECT_TRUE(pbr::OrgRelayFirstUseAllowed(false, true)) << "a dogfood build's compiled backend, warned";
  EXPECT_FALSE(pbr::OrgRelayFirstUseAllowed(false, false)) << "a release build's custom backend: not used";
}

// Release-compatibility RC022: a tester build (PP_TESTER_BUILD) shares the store app's data on a phone, so it never
// offers, runs or resolves to prod — whatever the compiled origins hold — and a store build is unchanged.
namespace {

constexpr pbr::CompiledOrigins kTesterAll{.staging = "https://staging.example.test",
                                          .dev = "https://dev.example.test",
                                          .staging_relay_id = "relay:staging",
                                          .dev_relay_id = "relay:dev",
                                          .staging_relay_certificate = "c3RhZ2luZw==",
                                          .dev_relay_certificate = "ZGV2",
                                          .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kTesterDevOnly{
    .dev = "https://dev.example.test", .dev_relay_id = "relay:dev", .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kTesterNone{.kind = pbr::BuildKind::Tester};
// A tester build's struct that names prod anyway (CMake refuses that build): still never prod.
constexpr pbr::CompiledOrigins kTesterWithProdNamed{.prod = "https://prod.example.test",
                                                    .staging = "https://staging.example.test",
                                                    .prod_relay_id = "relay:prod",
                                                    .staging_relay_id = "relay:staging",
                                                    .prod_relay_certificate = "cHJvZA==",
                                                    .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kStoreAll{.prod = "https://prod.example.test",
                                         .staging = "https://staging.example.test",
                                         .dev = "https://dev.example.test",
                                         .prod_relay_id = "relay:prod",
                                         .staging_relay_id = "relay:staging",
                                         .dev_relay_id = "relay:dev",
                                         .prod_relay_certificate = "cHJvZA=="};

constexpr pbr::BackendEnv kAllEnvs[] = {pbr::BackendEnv::Prod, pbr::BackendEnv::Staging, pbr::BackendEnv::Dev,
                                        pbr::BackendEnv::Custom};

} // namespace

TEST(DeploymentProfileTesterBuildTest, ProductionIsNeverAvailableNorCompiledInATesterBuild) {
  for (const pbr::CompiledOrigins& compiled : {kTesterAll, kTesterDevOnly, kTesterNone, kTesterWithProdNamed}) {
    EXPECT_FALSE(pbr::BackendAvailableIn(pbr::BackendEnv::Prod, compiled));
    EXPECT_STREQ(pbr::CompiledOriginFor(pbr::BackendEnv::Prod, compiled), "");
    EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Custom, compiled));
  }
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Staging, kTesterAll));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Dev, kTesterAll));
  EXPECT_FALSE(pbr::BackendAvailableIn(pbr::BackendEnv::Staging, kTesterDevOnly));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Prod, kStoreAll)) << "a store build: as before";
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Prod, kNoOrigins)) << "a store build with no origin";
}

TEST(DeploymentProfileTesterBuildTest, ATesterBuildFallsBackToStagingThenDevThenCustomNeverProd) {
  EXPECT_EQ(pbr::FallbackBackendIn(kTesterAll), pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::FallbackBackendIn(kTesterDevOnly), pbr::BackendEnv::Dev);
  EXPECT_EQ(pbr::FallbackBackendIn(kTesterNone), pbr::BackendEnv::Custom) << "neither compiled: a custom backend only";
  EXPECT_EQ(pbr::FallbackBackendIn(kStoreAll), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::FallbackBackendIn(kNoOrigins), pbr::BackendEnv::Prod);

  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, kTesterAll), pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, kTesterDevOnly), pbr::BackendEnv::Dev);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Staging, kTesterDevOnly), pbr::BackendEnv::Dev);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, kTesterNone), pbr::BackendEnv::Custom);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Staging, kTesterNone), pbr::BackendEnv::Custom);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, kTesterWithProdNamed), pbr::BackendEnv::Staging);
  for (const pbr::BackendEnv env : {pbr::BackendEnv::Staging, pbr::BackendEnv::Dev, pbr::BackendEnv::Custom}) {
    EXPECT_EQ(pbr::ResolveLatchedBackend(env, kTesterAll), env) << "what it can run, it runs";
  }
  for (const pbr::CompiledOrigins& compiled : {kTesterAll, kTesterDevOnly, kTesterNone, kTesterWithProdNamed}) {
    for (const pbr::BackendEnv env : kAllEnvs) {
      EXPECT_NE(pbr::ResolveLatchedBackend(env, compiled), pbr::BackendEnv::Prod);
    }
  }
  // A store build, as before: what it cannot run is prod.
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Staging, kProdOnly), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, kStoreAll), pbr::BackendEnv::Prod);
}

// Changed by profile-model P2 (#741): a custom backend has no origin of its own to borrow — before, a tester build's
// custom backend took the first usable tester environment's origin, a store build's custom backend prod's.
TEST(DeploymentProfileTesterBuildTest, ATesterBuildsOriginIsNeverProds) {
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Custom, kTesterAll), "");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Custom, kTesterDevOnly), "");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Custom, kTesterNone), "");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Dev, kTesterAll), "https://dev.example.test");
  for (const pbr::BackendEnv env : kAllEnvs) {
    EXPECT_NE(pbr::ResolveBriefOrigin(false, nullptr, env, kTesterWithProdNamed), "https://prod.example.test");
  }
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Custom, kStoreAll), "")
      << "a store build's custom backend no longer borrows prod's origin (#741)";
}

// Changed by profile-model P2 (#742): a custom backend takes no relay identity from a compiled environment — it names its
// own relay id in its table, or none (fails closed). Before, a store build gave it prod's and a tester build staging's.
TEST(DeploymentProfileTesterBuildTest, ATesterBuildsCustomBackendNeverGetsProdsOrgRelayIdentity) {
  const auto custom = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Custom, kTesterAll);
  EXPECT_TRUE(custom.relay_id.empty() && custom.certificate_b64.empty());
  const auto custom_dev = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Custom, kTesterDevOnly);
  EXPECT_TRUE(custom_dev.relay_id.empty() && custom_dev.certificate_b64.empty());
  const auto none = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Custom, kTesterNone);
  EXPECT_TRUE(none.relay_id.empty() && none.certificate_b64.empty()) << "no tester environment: none";
  for (const pbr::BackendEnv env : kAllEnvs) {
    const auto identity = pbr::ResolveOrgRelayIdentity(false, nullptr, env, kTesterWithProdNamed);
    EXPECT_NE(identity.relay_id, "relay:prod");
    EXPECT_NE(identity.certificate_b64, "cHJvZA==");
  }
  EXPECT_EQ(pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Staging, kTesterAll).relay_id,
            "relay:staging");
  const auto store_custom = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Custom, kStoreAll);
  EXPECT_TRUE(store_custom.relay_id.empty() && store_custom.certificate_b64.empty())
      << "a store build's custom backend no longer inherits prod's relay id (#742)";
}

// A tester build offers and starts only on an environment it can use: compiled, and its relay id not the placeholder
// (its operator set not made yet: that relay is refused, so account setup there fails with no way out). Store builds
// ignore the relay ids here (release-compatibility RC022).
namespace {

constexpr pbr::CompiledOrigins kTesterStagingPendingDevSet{.staging = "https://staging.example.test",
                                                           .dev = "https://dev.example.test",
                                                           .staging_relay_id = pbr::kPendingRelayId,
                                                           .dev_relay_id = "relay:dev",
                                                           .dev_relay_certificate = "ZGV2",
                                                           .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kTesterBothSet{.staging = "https://staging.example.test",
                                              .dev = "https://dev.example.test",
                                              .staging_relay_id = "relay:staging",
                                              .dev_relay_id = "relay:dev",
                                              .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kTesterBothPending{.staging = "https://staging.example.test",
                                                  .dev = "https://dev.example.test",
                                                  .staging_relay_id = pbr::kPendingRelayId,
                                                  .dev_relay_id = pbr::kPendingRelayId,
                                                  .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kTesterStagingSetDevPending{.staging = "https://staging.example.test",
                                                           .dev = "https://dev.example.test",
                                                           .staging_relay_id = "relay:staging",
                                                           .dev_relay_id = pbr::kPendingRelayId,
                                                           .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kStoreAllPending{.prod = "https://prod.example.test",
                                                .staging = "https://staging.example.test",
                                                .dev = "https://dev.example.test",
                                                .prod_relay_id = pbr::kPendingRelayId,
                                                .staging_relay_id = pbr::kPendingRelayId,
                                                .dev_relay_id = pbr::kPendingRelayId};

} // namespace

TEST(DeploymentProfileUsableEnvTest, AFreshTesterBuildStartsOnTheFirstUsableEnvironment) {
  // A fresh install runs the fallback (Config::DefaultAppConfigFor, the latch before any config).
  EXPECT_EQ(pbr::FallbackBackendIn(kTesterStagingPendingDevSet), pbr::BackendEnv::Dev) << "staging pending: dev";
  EXPECT_EQ(pbr::FallbackBackendIn(kTesterBothSet), pbr::BackendEnv::Staging) << "both set: staging first";
  EXPECT_EQ(pbr::FallbackBackendIn(kTesterStagingSetDevPending), pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::FallbackBackendIn(kTesterBothPending), pbr::BackendEnv::Custom) << "none usable: custom only";
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Custom, kTesterBothPending), "")
      << "custom only: no compiled origin for the org's services";
  const auto none = pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Custom, kTesterBothPending);
  EXPECT_TRUE(none.relay_id.empty() && none.certificate_b64.empty());
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, kTesterStagingPendingDevSet), pbr::BackendEnv::Dev)
      << "a store build's saved prod, read by a tester build";
}

TEST(DeploymentProfileUsableEnvTest, ASavedChoiceATesterBuildCannotUseRunsTheFirstUsableOne) {
  // The user's half-done Staging setup: the saved choice stays (Config: unavailable_env), the run is Dev's.
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Staging, kTesterStagingPendingDevSet), pbr::BackendEnv::Dev);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Dev, kTesterStagingPendingDevSet), pbr::BackendEnv::Dev);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Dev, kTesterStagingSetDevPending), pbr::BackendEnv::Staging);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Staging, kTesterBothPending), pbr::BackendEnv::Custom);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Custom, kTesterStagingPendingDevSet), pbr::BackendEnv::Custom);
  for (const pbr::BackendEnv env : {pbr::BackendEnv::Staging, pbr::BackendEnv::Dev}) {
    EXPECT_EQ(pbr::ResolveLatchedBackend(env, kTesterBothSet), env) << "both usable: what is saved runs";
  }
}

TEST(DeploymentProfileUsableEnvTest, ATesterBuildListsOnlyTheEnvironmentsItCanUse) {
  EXPECT_FALSE(pbr::BackendAvailableIn(pbr::BackendEnv::Staging, kTesterStagingPendingDevSet));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Dev, kTesterStagingPendingDevSet));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Staging, kTesterBothSet));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Dev, kTesterBothSet));
  EXPECT_FALSE(pbr::BackendAvailableIn(pbr::BackendEnv::Staging, kTesterBothPending));
  EXPECT_FALSE(pbr::BackendAvailableIn(pbr::BackendEnv::Dev, kTesterBothPending));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Custom, kTesterBothPending));
  EXPECT_TRUE(pbr::BackendAvailableIn(pbr::BackendEnv::Dev, kTesterDevOnly)) << "a named relay id: usable";
}

TEST(DeploymentProfileUsableEnvTest, ATesterBuildTakesNoOriginOrRelayOfAnEnvironmentItCannotUse) {
  // Whatever backend is passed — the latched one is always usable — none of these takes staging's pending relay.
  for (const pbr::BackendEnv env : kAllEnvs) {
    if (env == pbr::BackendEnv::Custom) {
      continue; // a custom backend takes no compiled environment at all (#741, #742)
    }
    EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, env, kTesterStagingPendingDevSet), "https://dev.example.test")
        << pbr::BackendEnvName(env);
    const auto identity = pbr::ResolveOrgRelayIdentity(false, nullptr, env, kTesterStagingPendingDevSet);
    EXPECT_EQ(identity.relay_id, "relay:dev") << pbr::BackendEnvName(env);
    EXPECT_EQ(identity.certificate_b64, "ZGV2");
  }
}

TEST(DeploymentProfileUsableEnvTest, AStoreBuildIsUnchangedWithPendingRelayIds) {
  for (const pbr::BackendEnv env : kAllEnvs) {
    EXPECT_TRUE(pbr::BackendAvailableIn(env, kStoreAllPending)) << pbr::BackendEnvName(env);
    EXPECT_EQ(pbr::ResolveLatchedBackend(env, kStoreAllPending), env);
  }
  EXPECT_EQ(pbr::FallbackBackendIn(kStoreAllPending), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Staging, kStoreAllPending),
            "https://staging.example.test");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Custom, kStoreAllPending), "")
      << "a custom backend has no origin of its own (#741)";
  EXPECT_EQ(pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Staging, kStoreAllPending).relay_id,
            pbr::kPendingRelayId)
      << "a store build's pending relay fails closed where it is used, as before";
  EXPECT_EQ(pbr::ResolveOrgRelayIdentity(false, nullptr, pbr::BackendEnv::Prod, kStoreAllPending).relay_id,
            pbr::kPendingRelayId);
}

// This build's own facts follow its kind (the ctest run with -DPP_TESTER_BUILD=ON checks the tester side).
TEST(DeploymentProfileTesterBuildTest, ThisBuildsCompiledOriginsCarryItsKind) {
  const pbr::CompiledOrigins compiled = pbr::BuildCompiledOrigins();
  EXPECT_EQ(compiled.kind, pbr::ThisBuildKind());
  EXPECT_EQ(pbr::IsTesterBuild(), pbr::ThisBuildKind() == pbr::BuildKind::Tester);
  EXPECT_EQ(pbr::BackendAvailable(pbr::BackendEnv::Prod), !pbr::IsTesterBuild());
  pbr::ResetActiveBackendForTest();
  if (pbr::IsTesterBuild()) {
    EXPECT_STREQ(pbr::CompiledDefaultOrigin(), "") << "CMake refuses a prod origin in a tester build";
    EXPECT_STREQ(pbr::CompiledDefaultRelayCertificate(), "");
    EXPECT_FALSE(pbr::CompiledDefaultRelayId()[0] == 'r') << "no prod relay id: pending or none";
    EXPECT_NE(pbr::ActiveBackend(), pbr::BackendEnv::Prod) << "not even before the latch";
    pbr::LatchActiveBackend(pbr::BackendEnv::Prod);
    EXPECT_NE(pbr::ActiveBackend(), pbr::BackendEnv::Prod);
  } else {
    EXPECT_EQ(pbr::ActiveBackend(), pbr::BackendEnv::Prod);
  }
  pbr::ResetActiveBackendForTest();
}
