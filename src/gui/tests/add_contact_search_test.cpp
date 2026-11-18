// The phone's Add contact page is opened from Chats (New chat -> Add contact), where the Contacts tab is not the active
// one: the search must still be requested (it ran only from a Contacts-tab tick, so the page showed "searching" forever).
#include "foundation/i18n/LocalizationService.h"
#include "common/runtime/AppRuntime.h"
#include "gui/contacts/ContactsController.h"
#include "gui/contacts/ContactsTickGate.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

TEST(ContactsTickGateTest, TheAddContactPageTicksOverTheChatsTab) {
  EXPECT_TRUE(ShouldTickContacts(NavTab::Contacts, false, false));
  EXPECT_TRUE(ShouldTickContacts(NavTab::Sessions, false, true)) << "Add contact opened from Chats";
  EXPECT_TRUE(ShouldTickContacts(NavTab::Sessions, true, false)) << "a contact page sheet over Chats";
  EXPECT_TRUE(ShouldTickContacts(NavTab::Home, false, true));
  EXPECT_FALSE(ShouldTickContacts(NavTab::Sessions, false, false));
  EXPECT_FALSE(ShouldTickContacts(NavTab::Me, false, false));
}

class AddContactSearchTest : public ::testing::Test {
protected:
  void SetUp() override {
    ContactsController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ShellNavigationPorts nav;
    nav.push_layer = [](const PaneSpec&) { return 1; };
    controller().BindShellNavigation(nav);
    MessagingContactsPorts ports;
    ports.search_people = [this](const std::string& query) -> Roe<std::vector<mesh::DirectoryHit>> {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        queries_.push_back(query);
      }
      return next_ ? next_() : Roe<std::vector<mesh::DirectoryHit>>(std::vector<mesh::DirectoryHit>{});
    };
    controller().BindContactsPorts(ports);
    controller().OpenAddContactPage();
    ASSERT_TRUE(controller().AddContactPageOpen());
  }
  void TearDown() override {
    controller().OnAddContactDismissed();
    controller().BindContactsPorts({});
    controller().BindShellNavigation({});
    AppRuntime::Shutdown();
    ContactsController::ClearInstance();
  }

  ContactsController& controller() { return controller_; }

  /** What the main loop does each frame, for `ms` of wall time. */
  void Frames(const int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      if (ShouldTickContacts(NavTab::Sessions, controller().DetailSheetOpen(), controller().AddContactPageOpen())) {
        controller().Tick();
      }
      AppRuntime::RunUITasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  std::vector<std::string> Queries() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queries_;
  }

  ContactsController controller_;
  std::mutex mutex_;
  std::vector<std::string> queries_;
  std::function<Roe<std::vector<mesh::DirectoryHit>>()> next_;
};

TEST_F(AddContactSearchTest, ATypedQueryIsSearchedOnceWhileChatsIsTheActiveTab) {
  controller().TypeInAddContact("ada");
  EXPECT_TRUE(controller().AddSearching());
  Frames(100);
  EXPECT_TRUE(Queries().empty()) << "still inside the 300 ms wait";
  Frames(500);
  ASSERT_EQ(Queries().size(), 1u);
  EXPECT_EQ(Queries()[0], "ada");
  EXPECT_FALSE(controller().AddSearching()) << "an empty answer ends the searching state";
  EXPECT_TRUE(controller().AddNoResults());
}

TEST_F(AddContactSearchTest, AnEmptyOrBlankQuerySendsNothing) {
  controller().TypeInAddContact("   ");
  EXPECT_FALSE(controller().AddSearching());
  Frames(450);
  controller().TypeInAddContact("");
  Frames(450);
  EXPECT_TRUE(Queries().empty());
}

TEST_F(AddContactSearchTest, AFailedSearchEndsTheSearchingState) {
  next_ = []() -> Roe<std::vector<mesh::DirectoryHit>> { return Roe<std::vector<mesh::DirectoryHit>>(Error("down")); };
  controller().TypeInAddContact("ada");
  Frames(600);
  ASSERT_EQ(Queries().size(), 1u);
  EXPECT_FALSE(controller().AddSearching());
}

} // namespace
} // namespace pbr
