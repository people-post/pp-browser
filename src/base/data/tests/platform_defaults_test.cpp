#include "base/platform/Platform.h"
#include "base/data/PlatformDefaults.h"
#include "base/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

TEST(PlatformDefaultsTest, DesktopDefaultsMatchExpectedValues) {
  const pbr::AppConfig config = pbr::PlatformDefaults::For(pbr::PlatformKind::Desktop);
  EXPECT_EQ(config.llm.preset, "brief");
  EXPECT_EQ(config.llm.base_url, "https://www.brief.global/api/llm/v1");
  EXPECT_TRUE(config.llm.require_api_key);
  EXPECT_EQ(config.llm.model, "xai");
  EXPECT_EQ(config.search.provider, "duckduckgo");
  EXPECT_EQ(config.promoted_mcp.url, "https://www.brief.global/mcp");
  EXPECT_EQ(config.relay.base_url, "https://www.brief.global/api/relay");
  EXPECT_EQ(config.directory.base_url, "https://www.brief.global/api/relay");
  EXPECT_EQ(config.registration.base_url, "https://www.brief.global/api/relay");
  EXPECT_EQ(config.theme, "themes/base.rcss");
}

TEST(PlatformDefaultsTest, SandboxDefaultsUsePeoplePostOrigin) {
  pbr::SetSandboxMode(true);
  const pbr::AppConfig config = pbr::PlatformDefaults::For(pbr::PlatformKind::Desktop);
  EXPECT_EQ(config.llm.base_url, "https://www-en.qa.peoplepost.org/api/llm/v1");
  EXPECT_EQ(config.promoted_mcp.url, "https://www-en.qa.peoplepost.org/mcp");
  EXPECT_EQ(config.relay.base_url, "https://www-en.qa.peoplepost.org/api/relay");
  pbr::SetSandboxMode(false);
}
