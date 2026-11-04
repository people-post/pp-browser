#include "foundation/data/Config.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/platform/DeploymentProfile.h"

#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <fstream>
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

std::string WriteConfig(const char* name, const std::string& body) {
  const std::string path = (std::filesystem::temp_directory_path() / name).string();
  std::ofstream out(path);
  out << body;
  return path;
}

constexpr const char* kSavedConfig = R"({
  "config_version": 1,
  "llm": { "preset": "brief", "base_url": "https://saved.example.test/api/llm/v1", "model": "xai" },
  "relay": { "base_url": "https://saved.example.test/api/relay" },
  "directory": { "base_url": "https://saved.example.test/api/relay" },
  "registration": { "base_url": "https://saved.example.test/api/relay" },
  "promoted_mcp": { "url": "https://saved.example.test/mcp" }
})";

} // namespace

TEST(ConfigOriginTest, EnvOverridesSavedBackendUrls) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin("https://backend.example.test");
  const std::string path = WriteConfig("pp_browser_config_origin_env.json", kSavedConfig);
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->llm.base_url, "https://backend.example.test/api/llm/v1");
  EXPECT_EQ(loaded->relay.base_url, "https://backend.example.test/api/relay");
  EXPECT_EQ(loaded->registration.base_url, "https://backend.example.test/api/relay");
  EXPECT_EQ(loaded->promoted_mcp.url, "https://backend.example.test/mcp");
  std::filesystem::remove(path);
}

TEST(ConfigOriginTest, SavedUrlsWinOverCompiledDefaultWhenEnvUnset) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin(nullptr);
  const std::string path = WriteConfig("pp_browser_config_origin_saved.json", kSavedConfig);
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  // The brief preset always derives its LLM URL from the origin; the other endpoints keep saved values.
  EXPECT_EQ(loaded->llm.base_url, pbr::BriefLlmBaseUrl());
  EXPECT_EQ(loaded->relay.base_url, "https://saved.example.test/api/relay");
  EXPECT_EQ(loaded->promoted_mcp.url, "https://saved.example.test/mcp");
  std::filesystem::remove(path);
}

TEST(ConfigOriginTest, EnvLeavesCustomLlmEndpointAlone) {
  ScopedOrigin origin("https://backend.example.test");
  const std::string path = WriteConfig(
      "pp_browser_config_origin_custom.json",
      R"({ "config_version": 1, "llm": { "preset": "custom", "base_url": "https://proxy.example.test/v1" } })");
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->llm.base_url, "https://proxy.example.test/v1");
  EXPECT_EQ(loaded->relay.base_url, "https://backend.example.test/api/relay");
  std::filesystem::remove(path);
}

TEST(ConfigOriginTest, NoOriginLeavesBackendUrlsEmpty) {
  ScopedOrigin origin(nullptr);
  if (!std::string(pbr::CompiledDefaultOrigin()).empty()) {
    GTEST_SKIP() << "a compile-time default origin is configured";
  }
  const pbr::AppConfig defaults = pbr::Config::DefaultAppConfig();
  EXPECT_TRUE(defaults.llm.base_url.empty());
  EXPECT_TRUE(defaults.relay.base_url.empty());
  EXPECT_TRUE(defaults.directory.base_url.empty());
  EXPECT_TRUE(defaults.registration.base_url.empty());
  EXPECT_FALSE(defaults.promoted_mcp.IsConfigured());
}

namespace {

constexpr const char* kBackendProdConfig = R"({
  "config_version": 1,
  "backend": { "env": "prod" },
  "relay": { "base_url": "https://custom.example.test/api/relay" },
  "directory": { "base_url": "https://custom.example.test/api/relay" },
  "registration": { "base_url": "https://custom.example.test/api/relay" }
})";

} // namespace

TEST(ConfigBackendTest, BackendEnvParsesAndDefaultsToProd) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin(nullptr);
  const std::string path = WriteConfig("pp_browser_config_backend_default.json", R"({"config_version": 1})");
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->backend.env, "prod");
  std::filesystem::remove(path);
  pbr::ResetActiveBackendForTest();
}

TEST(ConfigBackendTest, ProdIgnoresSavedUrlsButKeepsThemForSave) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin(nullptr);
  const std::string path = WriteConfig("pp_browser_config_backend_prod.json", kBackendProdConfig);
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->relay.base_url, pbr::BriefRelayBaseUrl());
  EXPECT_EQ(loaded->registration.base_url, pbr::BriefRelayBaseUrl());
  EXPECT_TRUE(loaded->backend.has_saved_urls);
  EXPECT_EQ(loaded->backend.saved_relay.base_url, "https://custom.example.test/api/relay");

  const std::string out = (std::filesystem::temp_directory_path() / "pp_browser_config_backend_out.json").string();
  ASSERT_TRUE(static_cast<bool>(pbr::Config::SaveToFile(out, *loaded)));
  std::string text;
  {
    std::ifstream in(out);
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  EXPECT_NE(text.find("custom.example.test"), std::string::npos) << "saved custom URLs survive a save";
  std::filesystem::remove(path);
  std::filesystem::remove(out);
  pbr::ResetActiveBackendForTest();
}

TEST(ConfigBackendTest, CustomUsesSavedUrls) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin(nullptr);
  const std::string path = WriteConfig(
      "pp_browser_config_backend_custom.json",
      R"({"config_version": 1, "backend": {"env": "custom"},
          "relay": {"base_url": "https://custom.example.test/api/relay"}})");
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->relay.base_url, "https://custom.example.test/api/relay");
  EXPECT_FALSE(loaded->backend.has_saved_urls);
  std::filesystem::remove(path);
  pbr::ResetActiveBackendForTest();
}

TEST(ConfigBackendTest, EnvOriginBeatsCustomBackend) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin("https://backend.example.test");
  const std::string path = WriteConfig(
      "pp_browser_config_backend_env_wins.json",
      R"({"config_version": 1, "backend": {"env": "custom"},
          "relay": {"base_url": "https://custom.example.test/api/relay"}})");
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->relay.base_url, "https://backend.example.test/api/relay");
  EXPECT_EQ(loaded->backend.saved_relay.base_url, "https://custom.example.test/api/relay");
  std::filesystem::remove(path);
  pbr::ResetActiveBackendForTest();
}

TEST(ConfigBackendTest, FileWithoutBackendKeyAndOwnUrlsIsAdoptedAsCustom) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin(nullptr);
  const std::string path = WriteConfig("pp_browser_config_backend_legacy.json", kSavedConfig);
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->backend.env, "custom");
  EXPECT_EQ(loaded->relay.base_url, "https://saved.example.test/api/relay");
  std::filesystem::remove(path);
  pbr::ResetActiveBackendForTest();
}

TEST(ConfigBackendTest, ReloadAfterChoosingAnotherBackendKeepsTheLatchedOne) {
  pbr::ResetActiveBackendForTest();
  ScopedOrigin origin(nullptr);
  const std::string path = WriteConfig("pp_browser_config_backend_latch.json", kBackendProdConfig);
  ASSERT_TRUE(static_cast<bool>(pbr::Config::LoadFromFile(path)));
  const std::string switched = WriteConfig(
      "pp_browser_config_backend_latch2.json",
      R"({"config_version": 1, "backend": {"env": "custom"},
          "relay": {"base_url": "https://custom.example.test/api/relay"}})");
  auto loaded = pbr::Config::LoadFromFile(switched);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->backend.env, "custom") << "the pick is kept for the next start";
  EXPECT_EQ(pbr::ActiveBackend(), pbr::BackendEnv::Prod) << "this run keeps its backend";
  EXPECT_EQ(loaded->relay.base_url, pbr::BriefRelayBaseUrl());
  std::filesystem::remove(path);
  std::filesystem::remove(switched);
  pbr::ResetActiveBackendForTest();
}
