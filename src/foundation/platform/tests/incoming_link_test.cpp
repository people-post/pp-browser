#include "foundation/platform/IncomingLink.h"

#include "foundation/runtime/AppRuntime.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(IncomingLinkTest, ASchemeIsALinkAPathIsNot) {
  EXPECT_TRUE(IncomingLink::LooksLikeLink("pp-browser-contact-v1:eyJh"));
  EXPECT_TRUE(IncomingLink::LooksLikeLink("https://example.com"));
  EXPECT_FALSE(IncomingLink::LooksLikeLink("/Users/me/photo.png"));
  EXPECT_FALSE(IncomingLink::LooksLikeLink("C:\\photo.png")) << "a drive letter";
  EXPECT_FALSE(IncomingLink::LooksLikeLink("no scheme"));
  EXPECT_FALSE(IncomingLink::LooksLikeLink("1x:y"));
}

TEST(IncomingLinkTest, LinksBeforeTheHandlerWaitForIt) {
  AppRuntime::Initialize();
  std::vector<std::string> taken;
  IncomingLink::Deliver("pp-browser-contact-v1:first");
  AppRuntime::RunUITasks();
  IncomingLink::SetHandler([&](std::string link) { taken.push_back(std::move(link)); });
  IncomingLink::Deliver("pp-browser-contact-v1:second");
  ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
  EXPECT_EQ(taken, (std::vector<std::string>{"pp-browser-contact-v1:first", "pp-browser-contact-v1:second"}));
  IncomingLink::SetHandler({});
  AppRuntime::Shutdown();
}

} // namespace
} // namespace pbr
