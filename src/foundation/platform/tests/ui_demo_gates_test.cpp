// The UI demo app's gates (DEV/PLAN-ui-demo-data.md). The decisions are plain functions with the demo flag passed in,
// so the normal build covers what the demo build does; kUiDemoBuild only decides what the call sites pass.
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(UiDemoGatesTest, DemoOriginIsEmptyWhateverTheEnvironmentSays) {
  EXPECT_EQ(ResolveBriefOrigin(true, "https://backend.example.test", BackendEnv::Prod), "");
  EXPECT_EQ(ResolveBriefOrigin(true, "https://backend.example.test", BackendEnv::Qa), "");
  EXPECT_EQ(ResolveBriefOrigin(true, nullptr, BackendEnv::Custom), "");
}

TEST(UiDemoGatesTest, NormalOriginStillPrefersTheEnvironmentAndDropsTrailingSlashes) {
  EXPECT_EQ(ResolveBriefOrigin(false, "https://backend.example.test//", BackendEnv::Prod),
            "https://backend.example.test");
  EXPECT_EQ(ResolveBriefOrigin(false, "", BackendEnv::Prod), std::string(CompiledDefaultOrigin()));
  EXPECT_EQ(ResolveBriefOrigin(false, nullptr, BackendEnv::Qa), std::string(CompiledQaOrigin()));
}

TEST(UiDemoGatesTest, DemoDiagnosticsUrlIsEmptyEvenWithTheOverrideAndAnOrigin) {
  EXPECT_EQ(ResolveDiagnosticsUploadUrl(true, "http://127.0.0.1:9/diag", "https://backend.example.test"), "");
  EXPECT_EQ(ResolveDiagnosticsUploadUrl(true, nullptr, "https://backend.example.test"), "");
}

TEST(UiDemoGatesTest, NormalDiagnosticsUrlIsUnchanged) {
  EXPECT_EQ(ResolveDiagnosticsUploadUrl(false, "http://127.0.0.1:9/diag", "https://backend.example.test"),
            "http://127.0.0.1:9/diag");
  EXPECT_EQ(ResolveDiagnosticsUploadUrl(false, nullptr, "https://backend.example.test"),
            "https://backend.example.test/api/support/diagnostics");
  EXPECT_EQ(ResolveDiagnosticsUploadUrl(false, "", ""), "");
}

TEST(UiDemoGatesTest, DemoIgnoresTheStreamUrlOverrideAndNormalKeepsIt) {
  EXPECT_EQ(ResolveBriefStreamUrlOverride(true, "http://127.0.0.1:9/stream"), "");
  EXPECT_EQ(ResolveBriefStreamUrlOverride(false, "http://127.0.0.1:9/stream"), "http://127.0.0.1:9/stream");
  EXPECT_EQ(ResolveBriefStreamUrlOverride(false, nullptr), "");
}

TEST(UiDemoGatesTest, DemoHidesChangePin) {
  EXPECT_FALSE(ChangePinVisible(true));
  EXPECT_TRUE(ChangePinVisible(false));
}

TEST(UiDemoGatesTest, DemoHidesTheMockReplyBanner) {
  EXPECT_FALSE(MockReplyBannerVisible(true));
  EXPECT_TRUE(MockReplyBannerVisible(false));
}

TEST(UiDemoGatesTest, DemoHidesThePeerLinkStatus) {
  EXPECT_FALSE(PeerLinkStatusVisible(true));
  EXPECT_TRUE(PeerLinkStatusVisible(false));
}

TEST(UiDemoGatesTest, DemoUploadsNothing) {
  EXPECT_FALSE(UploadAllowed(true));
  EXPECT_TRUE(UploadAllowed(false));
}

TEST(UiDemoGatesTest, TheSenderIsNotCalledInTheDemo) {
  int calls = 0;
  const auto sender = [&calls] { ++calls; };  // stands in for the diagnostics POST and the crash-report worker
  EXPECT_FALSE(RunIfUploadAllowed(true, sender));
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(RunIfUploadAllowed(false, sender));
  EXPECT_EQ(calls, 1);
}

TEST(UiDemoGatesTest, DemoHidesTheBackendSwitch) {
  EXPECT_FALSE(BackendSwitchVisible(true));
  EXPECT_TRUE(BackendSwitchVisible(false));
}

} // namespace
} // namespace pbr
