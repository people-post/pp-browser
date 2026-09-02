#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

namespace {

class SandboxModeGuard {
public:
  explicit SandboxModeGuard(const bool enabled) { pbr::SetSandboxMode(enabled); }
  ~SandboxModeGuard() { pbr::SetSandboxMode(false); }
};

} // namespace

TEST(DeploymentProfileTest, ProductionDefaults) {
  SandboxModeGuard guard(false);
  EXPECT_FALSE(pbr::SandboxMode());
  EXPECT_STREQ(pbr::BriefOrigin(), pbr::kProductionBriefOrigin);
  EXPECT_EQ(pbr::BriefLlmBaseUrl(), "https://www.brief.global/api/llm/v1");
  EXPECT_EQ(pbr::BriefRelayBaseUrl(), "https://www.brief.global/api/relay");
  EXPECT_EQ(pbr::BriefMcpUrl(), "https://www.brief.global/mcp");
  EXPECT_STREQ(pbr::ProductDirBasename(), pbr::kProductDirName);
}

TEST(DeploymentProfileTest, SandboxDefaults) {
  SandboxModeGuard guard(true);
  EXPECT_TRUE(pbr::SandboxMode());
  EXPECT_STREQ(pbr::BriefOrigin(), pbr::kSandboxBriefOrigin);
  EXPECT_EQ(pbr::BriefLlmBaseUrl(), "https://www-en.qa.peoplepost.org/api/llm/v1");
  EXPECT_EQ(pbr::BriefRelayBaseUrl(), "https://www-en.qa.peoplepost.org/api/relay");
  EXPECT_EQ(pbr::BriefMcpUrl(), "https://www-en.qa.peoplepost.org/mcp");
  EXPECT_STREQ(pbr::ProductDirBasename(), pbr::kSandboxProductDirName);
}

TEST(DeploymentProfileTest, RewritesProductionBriefUrlsWhenSandbox) {
  SandboxModeGuard guard(true);
  EXPECT_EQ(pbr::RewriteBriefOriginUrl("https://www.brief.global/api/relay"),
            "https://www-en.qa.peoplepost.org/api/relay");
  EXPECT_EQ(pbr::RewriteBriefOriginUrl("https://api.openai.com/v1"), "https://api.openai.com/v1");
}

TEST(DeploymentProfileTest, ResolveSandboxFromArgv) {
  SandboxModeGuard guard(false);
  char arg0[] = "pp-browser";
  char arg1[] = "--sandbox";
  char* argv[] = {arg0, arg1};
  EXPECT_TRUE(pbr::ResolveSandboxModeFromLaunch(2, argv));
}
