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

TEST(DeploymentProfileTest, NoOriginYieldsEmptyUrls) {
  ScopedOrigin origin(nullptr);
  if (!std::string(pbr::CompiledDefaultOrigin()).empty()) {
    GTEST_SKIP() << "a compile-time default origin is configured";
  }
  EXPECT_TRUE(pbr::BriefOrigin().empty());
  EXPECT_TRUE(pbr::BriefLlmBaseUrl().empty());
  EXPECT_TRUE(pbr::BriefRelayBaseUrl().empty());
  EXPECT_TRUE(pbr::BriefMcpUrl().empty());
  EXPECT_TRUE(pbr::DiagnosticsUploadUrl().empty());
}
