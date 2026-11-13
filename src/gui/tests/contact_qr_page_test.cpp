// The QR page knows whose code it shows (mine / a friend's), shows the Share and Save buttons only where the platform
// has them, and says what a save came to (DEV/PLAN-fewer-taps-2.md commit 3).
#include "gui/contacts/ContactsController.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

class ContactQrPageTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() { ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR)); }

  void SetUp() override {
    LocalizationService::Instance().SetPreferredLanguage("en");
    ShellNavigationPorts nav;
    nav.push_layer = [this](const PaneSpec&) { return ++pushed_; };
    controller_.BindShellNavigation(nav);
    FlowCoordinatorPorts flow;
    flow.begin_modal = [this](int, std::function<bool()>, std::function<void()> on_cancel) {
      dismiss_ = std::move(on_cancel);
    };
    controller_.BindFlowCoordinator(flow);
    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { toasts_.push_back(message); };
    controller_.BindShellFeedback(feedback);
    BindPorts(/*share=*/true, ImageSaveKind::Photos);
  }

  void BindPorts(const bool share, const ImageSaveKind kind) {
    ContactsAccountPorts ports;
    ports.load_identity = []() {
      ProfileIdentityView view;
      view.ready = true;
      view.relay_id = "relay:me";
      view.nickname = "Me Myself";
      view.contact_locator = "mine-locator";
      return view;
    };
    if (share) {
      ports.share_image = [this](const std::vector<uint8_t>& png, ShareAnchor) {
        shared_.push_back(png);
        return true;
      };
    }
    ports.save_image_kind = kind;
    ports.save_image = [this](const std::string& name, std::vector<uint8_t> png,
                              std::function<void(ImageSaveResult)> done) {
      saved_names_.push_back(name);
      saved_.push_back(std::move(png));
      (void)done; // the answer is fed to OnQrImageSaved by the tests
    };
    controller_.BindAccountPorts(ports);
  }

  ContactsController controller_;
  int pushed_ = 0;
  std::function<void()> dismiss_;
  std::vector<std::string> toasts_;
  std::vector<std::vector<uint8_t>> shared_;
  std::vector<std::vector<uint8_t>> saved_;
  std::vector<std::string> saved_names_;
};

TEST_F(ContactQrPageTest, AFriendsCodeThenMineShowsMineAndNeverTheFriendsLocator) {
  controller_.OpenQrPage(ContactsController::QrOwner::Friend, "Zed", "friend-locator", "scan to add Zed");
  EXPECT_EQ(controller_.QrPageOwner(), ContactsController::QrOwner::Friend);
  EXPECT_EQ(controller_.QrPageLocator(), "friend-locator");
  EXPECT_EQ(controller_.QrPageName(), "Zed");

  controller_.OpenMyQr(); // still open: the page changes in place
  EXPECT_EQ(controller_.QrPageOwner(), ContactsController::QrOwner::Mine);
  EXPECT_EQ(controller_.QrPageLocator(), "mine-locator");
  EXPECT_EQ(controller_.QrPageName(), "Me Myself");
  EXPECT_EQ(pushed_, 1) << "one page, not two stacked";
}

TEST_F(ContactQrPageTest, ClosedAndReopenedItShowsTheNewOwnerNotTheLastOne) {
  controller_.OpenQrPage(ContactsController::QrOwner::Friend, "Zed", "friend-locator", "");
  ASSERT_TRUE(dismiss_);
  dismiss_();
  controller_.OpenMyQr();
  EXPECT_EQ(pushed_, 2);
  EXPECT_EQ(controller_.QrPageOwner(), ContactsController::QrOwner::Mine);
  EXPECT_EQ(controller_.QrPageLocator(), "mine-locator");
}

TEST_F(ContactQrPageTest, ButtonsShowOnlyWhereThePlatformHasThem) {
  controller_.OpenMyQr();
  EXPECT_TRUE(controller_.QrPageCanShare());
  EXPECT_EQ(controller_.QrPageSaveLabel(), "Save to Photos");

  BindPorts(true, ImageSaveKind::File);
  controller_.OpenMyQr();
  EXPECT_EQ(controller_.QrPageSaveLabel(), "Save image…");

  BindPorts(false, ImageSaveKind::None); // Android, Windows, Linux: no sheet, no save
  controller_.OpenMyQr();
  EXPECT_FALSE(controller_.QrPageCanShare());
  EXPECT_EQ(controller_.QrPageSaveLabel(), "");
  controller_.OnShareQrImage({});
  controller_.OnSaveQrImage();
  EXPECT_TRUE(shared_.empty());
  EXPECT_TRUE(saved_.empty());
}

TEST_F(ContactQrPageTest, ShareHandsAPngFromMemoryToTheSheet) {
  controller_.OpenMyQr();
  controller_.OnShareQrImage({});
  ASSERT_EQ(shared_.size(), 1u);
  ASSERT_GT(shared_[0].size(), 8u);
  EXPECT_EQ(shared_[0][1], 'P');
  EXPECT_EQ(shared_[0][2], 'N');
  EXPECT_EQ(shared_[0][3], 'G');
  controller_.OnSaveQrImage();
  ASSERT_EQ(saved_.size(), 1u);
  EXPECT_EQ(saved_names_[0], "pp-qr-code.png");
}

TEST_F(ContactQrPageTest, EachSaveResultSaysItsOwnThing) {
  controller_.OpenMyQr();
  controller_.OnQrImageSaved(ImageSaveResult::Saved);
  controller_.OnQrImageSaved(ImageSaveResult::Denied);
  controller_.OnQrImageSaved(ImageSaveResult::Failed);
  controller_.OnQrImageSaved(ImageSaveResult::Cancelled); // closing the dialog says nothing
  ASSERT_EQ(toasts_.size(), 3u);
  EXPECT_EQ(toasts_[0], "Saved to Photos");
  EXPECT_EQ(toasts_[1], "Not saved: allow PP to add photos (Settings → PP → Photos)");
  EXPECT_EQ(toasts_[2], "Not saved, please try again");
}

TEST(ContactQrPageMarkupTest, ThePageNamesTheOwnerAndHasBothButtonsBehindTheirFlags) {
  const std::string rml = ReadAsset("views/my_qr.rml");
  EXPECT_NE(rml.find("{{qr_name}}"), std::string::npos);
  EXPECT_NE(rml.find("{{qr_hint}}"), std::string::npos);
  EXPECT_EQ(rml.find("{{my_name}}"), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"qr_can_share\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"share_qr_image()\""), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"qr_save_label != ''\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"save_qr_image()\""), std::string::npos);
}

} // namespace
} // namespace pbr
