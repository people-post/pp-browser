#include "base/platform/Platform.h"
#include "base/platform/PlatformDefaults.h"

#include <gtest/gtest.h>

TEST(PlatformDefaultsTest, DesktopDefaultsMatchExpectedValues) {
  const pbr::AppConfig config = pbr::PlatformDefaults::For(pbr::PlatformKind::Desktop);
  EXPECT_EQ(config.llm.preset, "brief");
  EXPECT_EQ(config.llm.base_url, "https://www.brief.global/api/llm/v1");
  EXPECT_FALSE(config.llm.require_api_key);
  EXPECT_EQ(config.llm.model, "grok-4-1-fast-reasoning");
  EXPECT_EQ(config.search.provider, "duckduckgo");
  EXPECT_EQ(config.promoted_mcp.url, "https://www.brief.global/mcp");
  EXPECT_EQ(config.relay.base_url, "https://www.brief.global/api/relay");
  EXPECT_EQ(config.directory.base_url, "https://www.brief.global/api/relay");
  EXPECT_EQ(config.registration.base_url, "https://www.brief.global/api/relay");
  EXPECT_EQ(config.theme, "themes/base.rcss");
}
