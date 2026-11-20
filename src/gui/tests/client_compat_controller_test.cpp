// Client compatibility (docs/contracts/SERVICE_ENDPOINTS.md § Client compatibility): checked at start-up from a fresh
// cache, again when the app comes to the foreground (forced, at most once per 15 minutes since the last fetch) and
// every 6 hours while it runs; the full screen below the document's floor, and the update card held up when the org's
// relay answers 426 `client_upgrade_required` — whatever the document's floor says.
#include "gui/ClientCompatController.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/Platform.h"
#include "foundation/product/AppVersion.h"
#include "gui/UpdateLink.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/RemoteSwitches.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kFetchedAt = 1'700'000'000;

/** The org relay's client-compat answer, counting fetches (any thread). */
class FakeCompatClient : public IClientCompatClient {
public:
  Roe<ClientCompatDocument> Fetch() override {
    ++fetches;
    return doc;
  }
  ClientCompatDocument doc;
  std::atomic<int> fetches{0};
};

struct Alert {
  std::string title;
  std::string message;
  std::string ok_label;
};

class ClientCompatControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_client_compat_controller_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    relay_->doc.schema_version = 1;
    controller_.SetClock([this]() { return now_; });
    controller_.BindShellFeedback(ShellFeedbackPorts{
        .show_alert = [this](const std::string& title, const std::string& message, std::function<void()>,
                             const std::string& ok_label) { alerts_.push_back({title, message, ok_label}); },
        .dialog_active = [this]() { return dialog_open_; },
    });
  }
  void TearDown() override {
    controller_.BindCompatPorts({});
    AppRuntime::Shutdown();
    std::filesystem::remove_all(dir_);
  }

  /** Messaging ready, the org relay's client and the profile's cache; `upgrade_required` receives the hub's hook. */
  void BindMessaging() {
    controller_.BindCompatPorts(MessagingCompatPorts{
        .messaging_initialized = []() { return true; },
        .client_compat = [this]() { return std::shared_ptr<IClientCompatClient>(relay_); },
        .profile_data_dir = [this]() { return dir_.string(); },
        .watch_upgrade_required = [this](std::function<void()> hook) { upgrade_hook_ = std::move(hook); },
    });
  }

  void CacheDocument(const ClientCompatDocument& doc, const int64_t fetched_at) {
    ASSERT_TRUE(SaveClientCompatCache(dir_.string(), ClientCompatCacheEntry{fetched_at, doc}));
  }

  /** Runs UI tasks until `done` (bounded). */
  void RunUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      AppRuntime::RunUITasks();
    }
    ASSERT_TRUE(done());
  }

  /** The check in flight answered (applied on the UI thread). */
  void Settle() {
    RunUntil([this]() { return !controller_.CheckInFlight(); });
  }

  std::filesystem::path dir_;
  int64_t now_ = kFetchedAt + 60;
  bool dialog_open_ = false;
  std::shared_ptr<FakeCompatClient> relay_ = std::make_shared<FakeCompatClient>();
  std::vector<Alert> alerts_;
  std::function<void()> upgrade_hook_;
  ClientCompatController controller_;
};

// Start-up takes a fresh cache as it is; the app in front again forces a check only once the document is 15 minutes old.
TEST_F(ClientCompatControllerTest, TheForegroundForcesACheckAtMostOncePerFifteenMinutes) {
  BindMessaging();
  ClientCompatDocument cached;
  cached.schema_version = 1;
  CacheDocument(cached, kFetchedAt);
  ASSERT_TRUE(controller_.CheckAsync());
  Settle();
  EXPECT_EQ(relay_->fetches.load(), 0) << "a fresh cache at start-up, as before";
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::None);

  controller_.OnForeground(); // a minute after the cached copy was fetched
  EXPECT_FALSE(controller_.CheckInFlight());
  EXPECT_EQ(relay_->fetches.load(), 0);

  now_ = kFetchedAt + kClientCompatRecheckSeconds;
  relay_->doc.min_client_version = "999.0.0";
  controller_.OnForeground();
  EXPECT_TRUE(controller_.CheckInFlight());
  Settle();
  EXPECT_EQ(relay_->fetches.load(), 1) << "forced past the cache, still fresh for start-up's 6 hours";
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::UpdateRequired);
  ASSERT_EQ(alerts_.size(), 1u);

  dialog_open_ = true;
  now_ += 60;
  controller_.OnForeground();
  EXPECT_FALSE(controller_.CheckInFlight()) << "fetched a minute ago";
  EXPECT_EQ(relay_->fetches.load(), 1);
}

// T15: while the app runs, its timer forces a check once the document held is six hours old, not before.
TEST_F(ClientCompatControllerTest, TheRunningAppChecksAgainEverySixHours) {
  BindMessaging();
  ClientCompatDocument cached;
  cached.schema_version = 1;
  CacheDocument(cached, kFetchedAt);
  ASSERT_TRUE(controller_.CheckAsync());
  Settle();
  EXPECT_EQ(relay_->fetches.load(), 0);
  now_ = kFetchedAt + kClientCompatTimerRecheckSeconds - 1;
  controller_.OnRecheckTimer();
  EXPECT_FALSE(controller_.CheckInFlight()) << "not six hours old yet";
  now_ = kFetchedAt + kClientCompatTimerRecheckSeconds;
  relay_->doc.min_client_version = "999.0.0";
  controller_.OnRecheckTimer();
  EXPECT_TRUE(controller_.CheckInFlight());
  Settle();
  EXPECT_EQ(relay_->fetches.load(), 1) << "a floor raised while the app runs is seen";
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::UpdateRequired);
}

// O2: the run's switches are the first document's; a later one is cached for the next start, not applied now.
TEST_F(ClientCompatControllerTest, TheRunTakesItsSwitchesFromItsFirstDocument) {
  RemoteSwitches::ResetForTest();
  BindMessaging();
  relay_->doc.switches = {{"test.flag", true}};
  ASSERT_TRUE(controller_.CheckAsync());
  Settle();
  EXPECT_TRUE(RemoteSwitches::IsOn("test.flag", false));
  relay_->doc.switches = {{"test.flag", false}};
  ASSERT_TRUE(controller_.CheckAsync(CompatCheck::Forced));
  Settle();
  EXPECT_TRUE(RemoteSwitches::IsOn("test.flag", false)) << "the next start takes the new value";
  auto cached = LoadClientCompatCache(dir_.string());
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->document.switches.at("test.flag"), false) << "kept for the next start";
  RemoteSwitches::ResetForTest();
}

// The org's relay refusing us (426) is the authority, even though the document raises no floor: the Chats page's card,
// held up (no "Later") with the document's link for this platform — never the full screen, and no alert at all, so a
// refusal at every poll raises nothing new (RC018 part 2 default; release-compatibility T14).
TEST_F(ClientCompatControllerTest, TheOrgRelaysUpgradeRequiredHoldsTheCardUpWithoutAnAlert) {
  BindMessaging();
  ASSERT_TRUE(upgrade_hook_) << "the controller listens to the org relay's 426";
  int announced = 0;
  controller_.BindSoftUpdateChanged([&announced]() { ++announced; });
  relay_->doc.upgrade_url = "https://github.com/people-post/pp-browser/releases";
  relay_->doc.upgrade_urls = {{Platform::OsName(), "https://apps.apple.com/app/id123"}};
  relay_->doc.message = "Relay 1.0 is live";
  upgrade_hook_(); // a relay call's thread: handled on the UI thread
  RunUntil([this]() { return controller_.SoftUpdate().has_value(); });
  Settle();
  EXPECT_EQ(relay_->fetches.load(), 1) << "forced, for the document's links";
  EXPECT_TRUE(alerts_.empty()) << "no full screen: the document raises no floor";
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::UpdateNeeded);
  ASSERT_TRUE(controller_.SoftUpdate().has_value());
  EXPECT_TRUE(controller_.SoftUpdate()->held);
  EXPECT_EQ(controller_.SoftUpdate()->url, "https://apps.apple.com/app/id123") << "this platform's link";
  EXPECT_EQ(controller_.SoftUpdate()->message, "Relay 1.0 is live") << "the relay's note under the card's own line";
  EXPECT_EQ(controller_.UpgradeUrl(), "https://apps.apple.com/app/id123");
  const int raised = announced;

  // Every later refusal (one per poll) changes nothing on screen and asks the relay nothing (it answered a moment ago).
  for (int i = 0; i < 5; ++i) {
    upgrade_hook_();
    AppRuntime::RunUITasks();
  }
  EXPECT_FALSE(controller_.CheckInFlight());
  EXPECT_EQ(relay_->fetches.load(), 1);
  EXPECT_TRUE(alerts_.empty());
  EXPECT_EQ(announced, raised) << "raised once";
  // A document without a floor keeps it held; one below whose floor this app is shows the full screen instead.
  controller_.ApplyDocument(relay_->doc);
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::UpdateNeeded);
  EXPECT_TRUE(controller_.SoftUpdate()->held);
  relay_->doc.min_client_version = "999.0.0";
  controller_.ApplyDocument(relay_->doc);
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::UpdateRequired);
  EXPECT_EQ(alerts_.size(), 1u) << "below the floor: the full screen";
  EXPECT_FALSE(controller_.SoftUpdate().has_value()) << "the dialog says it";
  controller_.BindSoftUpdateChanged({});
}

// Messaging not ready (nothing to fetch with): the card is still held up, with the app's own releases page.
TEST_F(ClientCompatControllerTest, WithoutAFetchTheCardIsStillHeldWithTheDefaultLink) {
  controller_.OnOrgRelayRequiresUpgrade();
  EXPECT_TRUE(alerts_.empty());
  ASSERT_TRUE(controller_.SoftUpdate().has_value());
  EXPECT_TRUE(controller_.SoftUpdate()->held);
  EXPECT_EQ(controller_.UpgradeUrl(), kDefaultUpgradeUrl);
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::UpdateNeeded);
}

// The relay sends no `message` (what prod and QA answer today): the app's own words, in the UI language.
TEST_F(ClientCompatControllerTest, WithoutARelayMessageTheScreenSaysItInTheAppsOwnWords) {
  BindMessaging();
  relay_->doc.min_client_version = "999.0.0";
  ASSERT_TRUE(relay_->doc.message.empty());
  ASSERT_TRUE(controller_.CheckAsync());
  Settle();
  ASSERT_EQ(alerts_.size(), 1u);
  EXPECT_EQ(alerts_[0].message, "This version of PP is no longer supported. Update to keep using it.");
  EXPECT_EQ(alerts_[0].message, Tr("compat.update_required.message"));

  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  dialog_open_ = false;
  controller_.ApplyDocument(relay_->doc);
  ASSERT_EQ(alerts_.size(), 2u);
  EXPECT_EQ(alerts_[1].message, "这个版本的 PP 已经不再支持，请更新后继续使用。");
  EXPECT_EQ(alerts_[1].title, "需要更新");
  EXPECT_EQ(alerts_[1].ok_label, "更新");
}

// The relay sends a `message`: the app's own words stay, the relay's is a second paragraph (RC018 part 2).
TEST_F(ClientCompatControllerTest, ARelayMessageIsAnExtraParagraphAfterTheAppsOwnWords) {
  BindMessaging();
  relay_->doc.min_client_version = "999.0.0";
  relay_->doc.message = "Maintenance: update by Friday";
  ASSERT_TRUE(controller_.CheckAsync());
  Settle();
  ASSERT_EQ(alerts_.size(), 1u);
  EXPECT_EQ(alerts_[0].message, Tr("compat.update_required.message") + "\n\nMaintenance: update by Friday");
  EXPECT_EQ(alerts_[0].title, "Update required") << "the title and button stay the app's";
}

TEST_F(ClientCompatControllerTest, ALongOrMarkedUpRelayMessageIsCappedAndKeptAsPlainText) {
  BindMessaging();
  relay_->doc.min_client_version = "999.0.0";
  relay_->doc.message = "<b>Update</b> now\n\n[link](https://x.test) & " + std::string(400, 'z');
  ASSERT_TRUE(controller_.CheckAsync());
  Settle();
  ASSERT_EQ(alerts_.size(), 1u);
  const std::string own = Tr("compat.update_required.message");
  ASSERT_EQ(alerts_[0].message.compare(0, own.size(), own), 0) << "the app's line first, never replaced";
  EXPECT_NE(alerts_[0].message.find("\n\n<b>Update</b> now [link](https://x.test) & zzz"), std::string::npos);
  EXPECT_NE(alerts_[0].message.find("\xE2\x80\xA6"), std::string::npos) << "capped with an ellipsis";
}

// A newer version than this one: the Chats list's card (the advice), no toast and no dialog; the card does not depend
// on the link, which only decides the Update button.
TEST_F(ClientCompatControllerTest, ANewerVersionRaisesTheCardWithoutAnAlertAndTheLinkOnlyDecidesTheButton) {
  BindMessaging();
  int changes = 0;
  controller_.BindSoftUpdateChanged([&changes]() { ++changes; });
  EXPECT_FALSE(controller_.SoftUpdate().has_value());

  relay_->doc.latest_client_version = "999.0.0";
  relay_->doc.upgrade_url = "https://example.test/releases"; // what prod answers before the store links are filled in
  ASSERT_TRUE(controller_.CheckAsync());
  Settle();
  EXPECT_EQ(controller_.LastAction(), CompatUiAction::SoftUpdateAvailable);
  EXPECT_TRUE(alerts_.empty());
  ASSERT_TRUE(controller_.SoftUpdate().has_value()) << "the card shows whatever the link";
  EXPECT_EQ(controller_.SoftUpdate()->latest_version, "999.0.0");
  EXPECT_TRUE(controller_.SoftUpdate()->message.empty());
  const std::string placeholder_link = UsableUpdateLink(relay_->doc, Platform::OsName(), kDefaultUpgradeUrl);
  EXPECT_EQ(controller_.SoftUpdate()->url, placeholder_link);
  const std::string os = Platform::OsName();
  EXPECT_EQ(placeholder_link.empty(), !(os == "macos" || os == "windows")) << "the releases page on the desktop only";
  EXPECT_EQ(changes, 1);

  relay_->doc.upgrade_url = "https://apps.apple.com/app/id123";
  controller_.ApplyDocument(relay_->doc);
  EXPECT_EQ(controller_.SoftUpdate()->url, "https://apps.apple.com/app/id123");
  EXPECT_EQ(changes, 2);
  controller_.ApplyDocument(relay_->doc);
  EXPECT_EQ(changes, 2) << "the same advice is not announced twice";
}

TEST_F(ClientCompatControllerTest, TheCardCarriesTheRelaysMessageAndAChangedMessageIsAnnounced) {
  BindMessaging();
  int changes = 0;
  controller_.BindSoftUpdateChanged([&changes]() { ++changes; });
  relay_->doc.latest_client_version = "999.0.0";
  relay_->doc.message = "Fixes calls";
  controller_.ApplyDocument(relay_->doc);
  ASSERT_TRUE(controller_.SoftUpdate().has_value());
  EXPECT_EQ(controller_.SoftUpdate()->message, "Fixes calls");
  EXPECT_EQ(changes, 1);
  relay_->doc.message = "Fixes calls and sound";
  controller_.ApplyDocument(relay_->doc);
  EXPECT_EQ(changes, 2);
}

TEST_F(ClientCompatControllerTest, TheCardGoesWhenUpToDateAndTheDialogCaseKeepsItsOwnLink) {
  BindMessaging();
  relay_->doc.latest_client_version = "999.0.0";
  controller_.ApplyDocument(relay_->doc);
  ASSERT_TRUE(controller_.SoftUpdate().has_value());
  relay_->doc.latest_client_version = "";
  controller_.ApplyDocument(relay_->doc);
  EXPECT_FALSE(controller_.SoftUpdate().has_value());

  relay_->doc.latest_client_version = "999.0.0";
  controller_.ApplyDocument(relay_->doc);
  ASSERT_TRUE(controller_.SoftUpdate().has_value());
  relay_->doc.min_client_version = "999.0.0";
  relay_->doc.upgrade_url = "https://example.test/releases";
  controller_.ApplyDocument(relay_->doc);
  EXPECT_FALSE(controller_.SoftUpdate().has_value()) << "below the floor: the dialog, not the card";
  ASSERT_EQ(alerts_.size(), 1u);
  EXPECT_EQ(controller_.UpgradeUrl(), "https://example.test/releases") << "the dialog still takes the resolved link";
}

} // namespace
} // namespace pbr
