// The big profile photo (phone profile page): its markup, the profile page's camera badge and avatar tap, and the
// viewer layer opening and closing.
#include "gui/avatar/AvatarViewController.h"
#include "lib/ui/ViewCatalog.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

TEST(AvatarViewMarkupTest, TheViewerHasACloseButtonATapAnywhereCloseAndThePictureFromTheBinding) {
  const std::string rml = ReadAsset("views/avatar_view.rml");
  ASSERT_FALSE(rml.empty());
  EXPECT_NE(rml.find("data-model=\"avatar_view\""), std::string::npos);
  // Tap anywhere (the root) and the close button both close.
  EXPECT_NE(rml.find("<div class=\"avatar-view\" data-model=\"avatar_view\" data-event-click=\"avatar_view_close()\""),
            std::string::npos);
  EXPECT_NE(rml.find("<button class=\"avatar-view-close shell-circle-btn\" type=\"button\" "
                     "data-event-click=\"avatar_view_close()\""),
            std::string::npos);
  EXPECT_NE(rml.find("title=\"{{i18n:common.close}}\""), std::string::npos);
  EXPECT_NE(rml.find("data-attr-src=\"avatar_view_src\""), std::string::npos);
  EXPECT_EQ(ViewCatalog::ResolvePath("avatar_view"), "views/avatar_view.rml");
}

TEST(AvatarViewMarkupTest, TheViewerShowsTheSquareAsStoredNotCircular) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t at = css.find(".avatar-view-image {");
  ASSERT_NE(at, std::string::npos);
  const std::string rule = css.substr(at, css.find('}', at) - at);
  EXPECT_NE(rule.find("max-width: 512dp"), std::string::npos);
  EXPECT_NE(rule.find("max-height: 512dp"), std::string::npos);
  EXPECT_EQ(rule.find("border-radius"), std::string::npos) << "no circular mask";
}

TEST(AvatarViewMarkupTest, TheProfilePhotoTapsTheViewerOrMenuAndTheCameraBadgeIsTheOnlyChangeButton) {
  const std::string rml = ReadAsset("views/settings_section_profile.rml");
  EXPECT_NE(rml.find("<div class=\"me-profile-avatar-frame\" data-event-click=\"tap_profile_avatar()\""),
            std::string::npos);
  const size_t badge = rml.find("<button class=\"me-profile-avatar-badge\"");
  ASSERT_NE(badge, std::string::npos);
  const std::string tag = rml.substr(badge, rml.find('>', badge) - badge);
  EXPECT_NE(tag.find("data-event-click=\"pick_profile_icon()\""), std::string::npos);
  EXPECT_NE(tag.find("data-attrif-disabled=\"profile_icon_uploading\""), std::string::npos);
  EXPECT_NE(rml.find("../icons/v2/camera.svg"), std::string::npos);
  // The camera badge is the only visible way to change the photo: no "Change photo" button any more.
  EXPECT_EQ(rml.find("settings.profile.change_photo"), std::string::npos);
  EXPECT_EQ(rml.find("me-profile-avatar-btn"), std::string::npos);
}

TEST(AvatarViewMarkupTest, ThePhoneProfileRulesMakeThePhoto112AndCentredAndTheDesktopKeeps64) {
  const std::string css = ReadAsset("themes/components.rcss");
  auto rule = [&](const std::string& selector) {
    const size_t at = css.find("\n" + selector + " {");
    EXPECT_NE(at, std::string::npos) << selector;
    return css.substr(at, css.find('}', at) - at);
  };
  EXPECT_NE(rule(".me-profile-avatar-wrap").find("width: 64dp"), std::string::npos);
  EXPECT_NE(rule(".shell-layer-compact .me-profile-avatar-wrap").find("width: 112dp"), std::string::npos);
  EXPECT_NE(rule(".shell-layer-compact .me-profile-avatar-wrap .me-profile-avatar-frame").find("border-radius: 56dp"),
            std::string::npos);
  EXPECT_NE(rule(".shell-layer-compact .me-profile-avatar-uploading").find("width: 112dp"), std::string::npos);
  EXPECT_NE(rule(".shell-layer-compact .me-profile-avatar-row").find("align-items: center"), std::string::npos);
  // The badge shows everywhere: 24dp with a 2dp ring on the desktop's 64dp photo, 36dp on the phone's 112dp one.
  const std::string desktop_badge = rule(".me-profile-avatar-badge");
  EXPECT_NE(desktop_badge.find("display: flex"), std::string::npos);
  EXPECT_NE(desktop_badge.find("width: 24dp"), std::string::npos);
  EXPECT_NE(desktop_badge.find("border-width: 2dp"), std::string::npos);
  const std::string badge = rule(".shell-layer-compact .me-profile-avatar-badge");
  EXPECT_NE(badge.find("width: 36dp"), std::string::npos);
}

TEST(AvatarViewControllerTest, OpenPushesAFullScreenLayerOnceAndCloseClosesItOnce) {
  AvatarViewController viewer;
  int pushed = 0;
  int closed = 0;
  bool full_screen = false;
  std::string key;
  ShellNavigationPorts navigation;
  navigation.push_layer = [&](const PaneSpec& spec) {
    ++pushed;
    full_screen = spec.full_screen;
    key = spec.key;
    return 9;
  };
  navigation.close_layer = [&](int id) {
    EXPECT_EQ(id, 9);
    ++closed;
  };
  viewer.BindShellNavigation(std::move(navigation));
  int ended = 0;
  FlowCoordinatorPorts flow;
  flow.begin_modal = [](int, std::function<bool()>, std::function<void()>) {};
  flow.end_modal = [&]() { ++ended; };
  viewer.BindFlowCoordinator(std::move(flow));

  viewer.Open("");
  EXPECT_EQ(pushed, 0) << "no picture, no viewer";
  viewer.Open("/tmp/icon.jpg");
  EXPECT_EQ(pushed, 1);
  EXPECT_TRUE(full_screen);
  EXPECT_EQ(key, "avatar_view");
  EXPECT_TRUE(viewer.IsOpen());
  viewer.Open("/tmp/icon.jpg");
  EXPECT_EQ(pushed, 1) << "one viewer at a time";
  viewer.Close();
  viewer.Close();
  EXPECT_EQ(closed, 1);
  EXPECT_EQ(ended, 1);
  EXPECT_FALSE(viewer.IsOpen());
}

TEST(AvatarViewControllerTest, TheBackKeyClosesItThroughTheFlowCoordinator) {
  AvatarViewController viewer;
  ShellNavigationPorts navigation;
  navigation.push_layer = [](const PaneSpec&) { return 4; };
  viewer.BindShellNavigation(std::move(navigation));
  std::function<bool()> step_back;
  std::function<void()> cancel;
  FlowCoordinatorPorts flow;
  flow.begin_modal = [&](int, std::function<bool()> back, std::function<void()> on_cancel) {
    step_back = std::move(back);
    cancel = std::move(on_cancel);
  };
  viewer.BindFlowCoordinator(std::move(flow));
  viewer.Open("/tmp/icon.jpg");
  ASSERT_TRUE(viewer.IsOpen());
  ASSERT_TRUE(step_back);
  EXPECT_FALSE(step_back()) << "back dismisses, it does not step back";
  cancel();
  EXPECT_FALSE(viewer.IsOpen());
}

} // namespace
} // namespace pbr
