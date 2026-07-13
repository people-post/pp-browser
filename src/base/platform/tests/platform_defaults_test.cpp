#include "base/platform/Platform.h"
#include "base/platform/PlatformDefaults.h"

#include <gtest/gtest.h>

TEST(PlatformDefaultsTest, DesktopDefaultsMatchExpectedValues) {
  const pbr::AppConfig config = pbr::PlatformDefaults::For(pbr::PlatformKind::Desktop);
  EXPECT_EQ(config.llm.base_url, "https://api.openai.com/v1");
  EXPECT_TRUE(config.llm.require_api_key);
  EXPECT_EQ(config.llm.model, "gpt-4o-mini");
  EXPECT_EQ(config.search.provider, "duckduckgo");
  EXPECT_EQ(config.promoted_mcp.url, "https://www.brief.global/mcp");
  EXPECT_EQ(config.theme, "themes/base.rcss");
}
