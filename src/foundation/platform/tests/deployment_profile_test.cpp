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
  EXPECT_EQ(pbr::BriefOrigin(), std::string(pbr::CompiledDefaultOrigin()));
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
