#include "base/data/Config.h"
#include "base/data/LlmPreset.h"
#include "base/platform/DeploymentProfile.h"

#include <fstream>
#include <filesystem>
#include <gtest/gtest.h>

namespace {

class SandboxModeGuard {
public:
  explicit SandboxModeGuard(const bool enabled) { pbr::SetSandboxMode(enabled); }
  ~SandboxModeGuard() { pbr::SetSandboxMode(false); }
};

} // namespace

TEST(ConfigSandboxTest, LoadRewritesProductionBriefUrls) {
  SandboxModeGuard guard(true);
  const std::string path =
      (std::filesystem::temp_directory_path() / "pp_browser_config_sandbox_rewrite.json").string();
  {
    std::ofstream out(path);
    out << R"({
      "config_version": 1,
      "llm": {
        "preset": "brief",
        "base_url": "https://www.brief.global/api/llm/v1",
        "model": "grok-4-1-fast-reasoning"
      },
      "relay": { "base_url": "https://www.brief.global/api/relay" },
      "directory": { "base_url": "https://www.brief.global/api/relay" },
      "registration": { "base_url": "https://www.brief.global/api/relay" },
      "promoted_mcp": { "url": "https://www.brief.global/mcp" }
    })";
  }

  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->llm.base_url, "https://www-en.qa.peoplepost.org/api/llm/v1");
  EXPECT_EQ(loaded->relay.base_url, "https://www-en.qa.peoplepost.org/api/relay");
  EXPECT_EQ(loaded->promoted_mcp.url, "https://www-en.qa.peoplepost.org/mcp");
  EXPECT_EQ(pbr::ResolvePreset(*loaded), "brief");
}

TEST(ConfigSandboxTest, NormalizeBriefPresetUsesSandboxLlmUrl) {
  SandboxModeGuard guard(true);
  pbr::AppConfig config;
  config.llm.preset = "brief";
  config.llm.base_url = "https://www.brief.global/api/llm/v1";
  pbr::NormalizeLlmConfig(config);
  EXPECT_EQ(config.llm.base_url, "https://www-en.qa.peoplepost.org/api/llm/v1");
}
