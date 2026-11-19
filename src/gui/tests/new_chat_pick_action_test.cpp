// Settings → Send run logs picks its contact on the New chat page (DEV/PLAN-send-run-logs.md). The "after pick" action
// is one-shot: it runs once, for that pick, after the chat opened; and it can never reach a later ordinary New chat —
// not after the page was left (back, a tab change taking the page away), not after a contact who cannot be messaged
// was tapped, not when the page is opened again the ordinary way.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace pbr {
namespace {

class NewChatPickActionTest : public ::testing::Test {
protected:
  void SetUp() override {
    ContactsController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");

    ShellNavigationPorts nav;
    nav.snapshot = []() {
      ShellChromeSnapshot chrome;
      chrome.layout_mode = LayoutMode::Compact;
      return chrome;
    };
    nav.push_transient = [this](const PaneSpec& spec) { pages_.push_back(spec.key); };
    nav.pop_transient = [this]() { PopPage(); };
    nav.push_layer = [](const PaneSpec&) { return 1; };
    nav.select_nav_tab = [this](NavTab tab) {
      if (tab != tab_) {
        while (!pages_.empty()) {
          PopPage(); // a tab change takes the pushed pages away (ShellHost::ClearTabContext)
        }
      }
      tab_ = tab;
    };
    nav.set_primary_pane = [](const std::string&) {};
    controller_.BindShellNavigation(nav);

    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    store_.push_back(MakeContact("c-ann", "Ann", true));
    store_.push_back(MakeContact("c-bea", "Bea", true));
    store_.push_back(MakeContact("c-nobody", "Nobody", false)); // no account id: cannot be messaged yet
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      for (const Contact& contact : store_) {
        if (contact.id == id) {
          return std::optional<Contact>{contact};
        }
      }
      return std::optional<Contact>{};
    };
    ports.list_contacts = [this]() -> Roe<std::vector<Contact>> { return store_; };
    ports.find_or_create_direct_thread = [this](const std::string& id, ThreadChannel) -> Roe<Thread> {
      opened_.push_back(id);
      Thread thread;
      thread.id = "thread-of-" + id;
      return thread;
    };
    controller_.BindContactsPorts(ports);
    controller_.OnNavTabActivated();
  }
  void TearDown() override {
    controller_.BindContactsPorts({});
    controller_.BindShellNavigation({});
    AppRuntime::Shutdown();
    ContactsController::ClearInstance();
  }

  static Contact MakeContact(const std::string& id, const std::string& name, const bool reachable) {
    Contact contact;
    contact.id = id;
    contact.local.display_name = name;
    if (reachable) {
      contact.remote.ids = {{mesh::ContactIdKind::Account, "account:" + id, true}};
    }
    SyncContactMirrors(contact);
    return contact;
  }
  /** The shell pops the top page and tells the controller which (Application's SetOnTransientPopped). */
  void PopPage() {
    if (pages_.empty()) {
      return;
    }
    const std::string key = pages_.back();
    pages_.pop_back();
    if (key == "new_chat") {
      controller_.OnNewChatDismissed();
    }
  }
  /** Opens the page to pick for Send run logs; each run of the action is recorded. */
  void OpenToPick() {
    ASSERT_TRUE(controller_.OpenNewChatPageToPick([this](const std::string& id) { sent_to_.push_back(id); }));
    ASSERT_EQ(pages_, std::vector<std::string>{"new_chat"});
    ASSERT_TRUE(controller_.PickActionPending());
  }
  /** An ordinary New chat (the Chats "+") and a pick there. */
  void OrdinaryNewChatPick(const std::string& id) {
    tab_ = NavTab::Sessions;
    controller_.OpenNewChatPage();
    EXPECT_FALSE(controller_.PickActionPending());
    controller_.OnNewChatPickForTest(id);
  }

  ContactsController controller_;
  std::vector<Contact> store_;
  std::vector<std::string> pages_;
  std::vector<std::string> opened_;
  std::vector<std::string> sent_to_;
  NavTab tab_ = NavTab::Me; // Send run logs starts from Settings
};

TEST_F(NewChatPickActionTest, ThePickOpensThatChatAndRunsTheActionOnceForIt) {
  OpenToPick();
  controller_.OnNewChatPickForTest("c-ann");
  EXPECT_EQ(opened_, std::vector<std::string>{"c-ann"}) << "the chat opened first, as an ordinary pick does";
  EXPECT_EQ(sent_to_, std::vector<std::string>{"c-ann"});
  EXPECT_TRUE(pages_.empty());
  EXPECT_EQ(tab_, NavTab::Sessions);
  EXPECT_FALSE(controller_.PickActionPending());

  OrdinaryNewChatPick("c-bea");
  EXPECT_EQ(sent_to_, std::vector<std::string>{"c-ann"}) << "once only";
}

TEST_F(NewChatPickActionTest, BackOutOfThePageThenAnOrdinaryNewChatSendsNothing) {
  OpenToPick();
  PopPage(); // back / edge swipe
  EXPECT_FALSE(controller_.PickActionPending());
  OrdinaryNewChatPick("c-bea");
  EXPECT_EQ(opened_, std::vector<std::string>{"c-bea"});
  EXPECT_TRUE(sent_to_.empty());
}

TEST_F(NewChatPickActionTest, ATabChangeTakingThePageAwayDropsTheAction) {
  OpenToPick();
  while (!pages_.empty()) {
    PopPage(); // the tab bar: the shell clears the tab's pages
  }
  EXPECT_FALSE(controller_.PickActionPending());
  OrdinaryNewChatPick("c-ann");
  EXPECT_TRUE(sent_to_.empty());
}

TEST_F(NewChatPickActionTest, AContactWhoCannotBeMessagedClosesThePageAndDropsTheAction) {
  OpenToPick();
  controller_.OnNewChatPickForTest("c-nobody");
  EXPECT_TRUE(sent_to_.empty());
  EXPECT_TRUE(opened_.empty());
  EXPECT_TRUE(pages_.empty()) << "the page closes: it is not left half-armed";
  EXPECT_FALSE(controller_.PickActionPending());
  OrdinaryNewChatPick("c-ann");
  EXPECT_TRUE(sent_to_.empty());
}

TEST_F(NewChatPickActionTest, OpeningThePageTheOrdinaryWayWhileArmedDropsTheAction) {
  OpenToPick();
  controller_.OpenNewChatPage(); // e.g. the Chats "+" reached while it was up
  EXPECT_FALSE(controller_.PickActionPending());
  controller_.OnNewChatPickForTest("c-ann");
  EXPECT_TRUE(sent_to_.empty());
}

TEST_F(NewChatPickActionTest, AnOrdinaryUnreachablePickKeepsThePageAsBefore) {
  tab_ = NavTab::Sessions;
  controller_.OpenNewChatPage();
  controller_.OnNewChatPickForTest("c-nobody");
  EXPECT_EQ(pages_, std::vector<std::string>{"new_chat"}) << "an ordinary New chat stays, as it always did";
}

TEST_F(NewChatPickActionTest, NoPageNoAction) {
  ShellNavigationPorts none;
  controller_.BindShellNavigation(none);
  EXPECT_FALSE(controller_.OpenNewChatPageToPick([this](const std::string& id) { sent_to_.push_back(id); }));
  EXPECT_FALSE(controller_.PickActionPending());
}

} // namespace
} // namespace pbr
