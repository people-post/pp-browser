// A linked device its account took off its device list: the Chats page says so as soon as the device knows (not only
// after a send failed), once per launch, with "Link again" (the reset flow) and "OK"; a send in that state says why.
#include "feature/presenters/DeviceRemovedNotice.h"

#include "common/Localized.h"
#include "domain/peers/OwnCards.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

/** The app around the notice: what the device list says, its change hook, the reset flow, the two lanes. */
class DeviceRemovedNoticeTest : public ::testing::Test {
protected:
  std::unique_ptr<DeviceRemovedNotice> Launch() {
    auto notice = std::make_unique<DeviceRemovedNotice>([this] { ++redraws_; });
    DeviceRemovedNoticePorts ports;
    ports.signed_out = [this] {
      ++reads_;
      return signed_out_;
    };
    ports.watch = [this](std::function<void()> changed) { list_changed_ = std::move(changed); };
    ports.relink = [this] { ++relinks_; };
    ports.post_worker = [this](std::function<void()> task) { worker_.push_back(std::move(task)); };
    ports.post_ui = [this](std::function<void()> task) { ui_.push_back(std::move(task)); };
    notice->BindPorts(std::move(ports));
    return notice;
  }

  /** Runs what was posted, lane by lane, until nothing is left. */
  void Settle() {
    while (!worker_.empty() || !ui_.empty()) {
      auto worker = std::move(worker_);
      worker_.clear();
      for (auto& task : worker) {
        task();
      }
      auto ui = std::move(ui_);
      ui_.clear();
      for (auto& task : ui) {
        task();
      }
    }
  }

  /** The account's device list changes (a sibling's notice, a card fetch), as the hub reports it. */
  void ListChanges(const bool signed_out) {
    signed_out_ = signed_out;
    ASSERT_TRUE(list_changed_) << "the notice listens for the next change";
    list_changed_();
    Settle();
  }

  bool signed_out_ = false;
  int reads_ = 0;
  int redraws_ = 0;
  int relinks_ = 0;
  std::function<void()> list_changed_;
  std::vector<std::function<void()>> worker_;
  std::vector<std::function<void()>> ui_;
};

TEST_F(DeviceRemovedNoticeTest, ARemovedDeviceShowsTheNoticeAtTheMessagingStart) {
  signed_out_ = true;
  auto notice = Launch();
  notice->Check();
  EXPECT_FALSE(notice->Shows()) << "the list is read on a worker, the answer lands on the UI thread";
  Settle();
  EXPECT_TRUE(notice->Shows());
  EXPECT_EQ(redraws_, 1);
}

TEST_F(DeviceRemovedNoticeTest, AMainOrListedDeviceNeverShowsIt) {
  signed_out_ = false; // OwnDeviceList::signed_out is false on the main device and on a listed linked one
  auto notice = Launch();
  notice->Check();
  Settle();
  EXPECT_FALSE(notice->Shows());
  ListChanges(false);
  EXPECT_FALSE(notice->Shows());
  EXPECT_EQ(redraws_, 0);
  notice->Relink();
  EXPECT_EQ(relinks_, 0) << "nothing to link again";
}

TEST_F(DeviceRemovedNoticeTest, ARemovalHeardWhileRunningShowsItAtOnce) {
  auto notice = Launch();
  notice->Check();
  Settle();
  ASSERT_FALSE(notice->Shows());
  ListChanges(true); // the main device took this one off: its new list reached us
  EXPECT_TRUE(notice->Shows());
}

TEST_F(DeviceRemovedNoticeTest, OkHidesItForThisLaunchOnlyAndTheNextLaunchShowsItAgain) {
  signed_out_ = true;
  auto notice = Launch();
  notice->Check();
  Settle();
  ASSERT_TRUE(notice->Shows());
  notice->Ok();
  EXPECT_FALSE(notice->Shows());
  EXPECT_EQ(redraws_, 2);
  ListChanges(true);
  notice->Check(); // another messaging start in the same run (an unlock)
  Settle();
  EXPECT_FALSE(notice->Shows()) << "once per launch";

  auto next_launch = Launch();
  next_launch->Check();
  Settle();
  EXPECT_TRUE(next_launch->Shows());
}

TEST_F(DeviceRemovedNoticeTest, LinkAgainHandsOverToTheResetFlowAndTheNoticeGoesOnceListed) {
  signed_out_ = true;
  auto notice = Launch();
  notice->Check();
  Settle();
  notice->Relink();
  EXPECT_EQ(relinks_, 1) << "the reset flow asks first, then resets";
  EXPECT_TRUE(notice->Shows()) << "a cancelled confirm leaves it up";
  ListChanges(false); // linked again
  EXPECT_FALSE(notice->Shows());
}

TEST_F(DeviceRemovedNoticeTest, AnAnswerFromBeforeMessagingWentDownIsDropped) {
  signed_out_ = true;
  auto notice = Launch();
  notice->Check();
  notice->Forget(); // a lock or a profile reset before the answer came back
  Settle();
  EXPECT_FALSE(notice->Shows());
}

// "Link again" confirmed: the profile reset tears messaging down (ChatController::OnProfileDataReset -> Forget) and the
// card goes with the old profile, even with an answer still on its way; the empty profile behind the first-run page
// (the same process on a phone, a new one on the desktop) is on no device list, so the card does not come back.
TEST_F(DeviceRemovedNoticeTest, AfterLinkAgainTheResetHidesItAndTheEmptyProfileDoesNotBringItBack) {
  signed_out_ = true;
  auto notice = Launch();
  notice->Check();
  Settle();
  ASSERT_TRUE(notice->Shows());
  notice->Relink();
  ASSERT_EQ(relinks_, 1);
  notice->Check();  // a device-list change heard just before the wipe: its answer is still out
  notice->Forget(); // the reset
  EXPECT_FALSE(notice->Shows());
  Settle();
  EXPECT_FALSE(notice->Shows()) << "the old profile's answer is dropped";

  signed_out_ = false; // no account yet: messaging is not up, so the list reads as not signed out
  notice->Check();     // the same process: the next messaging start (a new account, or linked again)
  Settle();
  EXPECT_FALSE(notice->Shows());
  auto restarted = Launch(); // the desktop: a new process on the empty profile
  restarted->Check();
  Settle();
  EXPECT_FALSE(restarted->Shows());
}

TEST_F(DeviceRemovedNoticeTest, ASendFromARemovedDeviceSaysWhyInsteadOfTheGenericLine) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  const Error refused = SignedOutSendError(); // what MeshDeliveryOrchestrator::OwnSender returns
  EXPECT_EQ(AppError::Display(refused), "这台设备已不在你的账号里，发不出去。请重新关联。");
  EXPECT_NE(AppError::Display(refused), Localized("error.generic"));
  EXPECT_EQ(refused.message, kSignedOutMessage) << "the log keeps the developer's line";
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(AppError::Display(refused),
            "This device is no longer part of your account, so it can't send. Link it again.");
}

} // namespace
} // namespace pbr
