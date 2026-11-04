#include "foundation/platform/Platform.h"
#include "foundation/data/PlatformDefaults.h"
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

TEST(PlatformDefaultsTest, DesktopDefaultsMatchExpectedValues) {
  ScopedOrigin origin("https://backend.example.test");
  const pbr::AppConfig config = pbr::PlatformDefaults::For(pbr::PlatformKind::Desktop);
  EXPECT_EQ(config.llm.preset, "brief");
  EXPECT_EQ(config.llm.base_url, "https://backend.example.test/api/llm/v1");
  EXPECT_TRUE(config.llm.require_api_key);
  EXPECT_EQ(config.llm.model, "xai");
  EXPECT_EQ(config.search.provider, "duckduckgo");
  EXPECT_EQ(config.promoted_mcp.url, "https://backend.example.test/mcp");
  EXPECT_EQ(config.relay.base_url, "https://backend.example.test/api/relay");
  EXPECT_EQ(config.directory.base_url, "https://backend.example.test/api/relay");
  EXPECT_EQ(config.registration.base_url, "https://backend.example.test/api/relay");
  EXPECT_EQ(config.theme, "themes/base.rcss");
}

TEST(PlatformDefaultsTest, NoOriginLeavesBackendEndpointsEmpty) {
  ScopedOrigin origin(nullptr);
  if (!std::string(pbr::CompiledDefaultOrigin()).empty()) {
    GTEST_SKIP() << "a compile-time default origin is configured";
  }
  const pbr::AppConfig config = pbr::PlatformDefaults::For(pbr::PlatformKind::Desktop);
  EXPECT_TRUE(config.llm.base_url.empty());
  EXPECT_FALSE(config.promoted_mcp.IsConfigured());
  EXPECT_TRUE(config.relay.base_url.empty());
  EXPECT_TRUE(config.directory.base_url.empty());
  EXPECT_TRUE(config.registration.base_url.empty());
}
