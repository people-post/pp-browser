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
constexpr pbr::CompiledOrigins kProdAndQa{.prod = "https://prod.example.test", .qa = "https://qa.example.test"};

TEST(DeploymentProfileTest, NoOriginYieldsEmptyUrls) {
  const std::string origin = pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Prod, kNoOrigins);
  EXPECT_TRUE(origin.empty());
  EXPECT_TRUE(pbr::BriefLlmBaseUrlFor(origin).empty());
  EXPECT_TRUE(pbr::BriefRelayBaseUrlFor(origin).empty());
  EXPECT_TRUE(pbr::BriefMcpUrlFor(origin).empty());
  EXPECT_TRUE(pbr::ResolveDiagnosticsUploadUrl(false, nullptr, origin).empty());
}

TEST(DeploymentProfileTest, ACompiledDefaultOriginIsUsedWithoutTheEnvironment) {
  const std::string origin = pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Prod, kProdOnly);
  EXPECT_EQ(origin, "https://prod.example.test");
  EXPECT_EQ(pbr::BriefLlmBaseUrlFor(origin), "https://prod.example.test/api/llm/v1");
  EXPECT_EQ(pbr::BriefRelayBaseUrlFor(origin), "https://prod.example.test/api/relay");
  EXPECT_EQ(pbr::BriefMcpUrlFor(origin), "https://prod.example.test/mcp");
  EXPECT_EQ(pbr::ResolveDiagnosticsUploadUrl(false, nullptr, origin),
            "https://prod.example.test/api/support/diagnostics");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, "", pbr::BackendEnv::Prod, kProdOnly), "https://prod.example.test")
      << "an empty variable is unset";
}

TEST(DeploymentProfileTest, BackendEnvNamesRoundTripAndUnknownIsProd) {
  for (const auto env : {pbr::BackendEnv::Prod, pbr::BackendEnv::Qa, pbr::BackendEnv::Custom}) {
    EXPECT_EQ(pbr::ParseBackendEnv(pbr::BackendEnvName(env)), env);
  }
  EXPECT_EQ(pbr::ParseBackendEnv(""), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ParseBackendEnv("staging"), pbr::BackendEnv::Prod);
}

TEST(DeploymentProfileTest, ActiveBackendLatchesOnFirstCall) {
  pbr::ResetActiveBackendForTest();
  EXPECT_FALSE(pbr::ActiveBackendLatched());
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom);
  pbr::LatchActiveBackend(pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ActiveBackend(), pbr::BackendEnv::Custom);
  pbr::ResetActiveBackendForTest();
}

TEST(DeploymentProfileTest, QaFallsBackToProdWithoutCompiledQaOrigin) {
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Qa, false), pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Qa, true), pbr::BackendEnv::Qa);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Custom, false), pbr::BackendEnv::Custom);
  EXPECT_EQ(pbr::ResolveLatchedBackend(pbr::BackendEnv::Prod, true), pbr::BackendEnv::Prod);
}

// This build's own: the latch applies the rule with its compiled QA origin.
TEST(DeploymentProfileTest, TheLatchUsesThisBuildsQaOrigin) {
  pbr::ResetActiveBackendForTest();
  pbr::LatchActiveBackend(pbr::BackendEnv::Qa);
  EXPECT_EQ(pbr::ActiveBackend(), pbr::ResolveLatchedBackend(pbr::BackendEnv::Qa, pbr::QaBackendAvailable()));
  pbr::ResetActiveBackendForTest();
}

TEST(DeploymentProfileTest, QaBackendUsesQaOriginAndEnvStillWins) {
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Qa, kProdAndQa), "https://qa.example.test");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, nullptr, pbr::BackendEnv::Prod, kProdAndQa), "https://prod.example.test");
  EXPECT_EQ(pbr::ResolveBriefOrigin(false, "https://backend.example.test", pbr::BackendEnv::Qa, kProdAndQa),
            "https://backend.example.test");
}
