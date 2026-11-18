// "Message" on a contact's card opens the chat where the user can see it: the card (a full-screen page on phones) and the
// pages under it (Add contact, New chat) go, or the chat opens underneath and the button looks dead.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace pbr {
namespace {

class ContactCardMessageTest : public ::testing::Test {
protected:
  void SetUp() override {
    ContactsController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");

    ShellNavigationPorts nav;
    nav.snapshot = [this]() {
      ShellChromeSnapshot chrome;
      chrome.layout_mode = LayoutMode::Compact;
      chrome.contact_detail_transient =
          std::find(pages_.begin(), pages_.end(), std::string("contact_detail")) != pages_.end();
      return chrome;
    };
    nav.push_transient = [this](const PaneSpec& spec) { pages_.push_back(spec.key); };
    nav.pop_transient = [this]() {
      if (!pages_.empty()) {
        pages_.pop_back();
      }
    };
    nav.push_layer = [](const PaneSpec&) { return 1; };
    nav.select_nav_tab = [this](NavTab tab) { tab_ = tab; };
    nav.set_primary_pane = [this](const std::string& key) { pane_ = key; };
    controller_.BindShellNavigation(nav);

    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    Contact contact;
    contact.id = "c-ann";
    contact.display_name = "Ann";
    contact.remote.ids = {{mesh::ContactIdKind::Account, "account:ann", true}};
    SyncContactMirrors(contact);
    ports.get_contact = [contact](const std::string& id) -> Roe<std::optional<Contact>> {
      return id == contact.id ? std::optional<Contact>{contact} : std::optional<Contact>{};
    };
    ports.find_or_create_direct_thread = [this](const std::string& id, ThreadChannel) -> Roe<Thread> {
      opened_.push_back(id);
      Thread thread;
      thread.id = "thread-of-" + id;
      return thread;
    };
    controller_.BindContactsPorts(ports);
    controller_.OnNavTabActivated(); // reads the layout mode: a phone
  }
  void TearDown() override {
    controller_.OnAddContactDismissed();
    controller_.BindContactsPorts({});
    controller_.BindShellNavigation({});
    AppRuntime::Shutdown();
    ContactsController::ClearInstance();
  }

  ContactsController controller_;
  std::vector<std::string> pages_; // the shell's transient pages, top last
  std::vector<std::string> opened_;
  NavTab tab_ = NavTab::Contacts;
  std::string pane_;
};

TEST_F(ContactCardMessageTest, MessageOnTheCardClosesTheCardAndTheNewChatPageUnderIt) {
  controller_.OpenNewChatPage();
  controller_.OnSelectContact("c-ann"); // a search result's card, full screen over New chat
  ASSERT_EQ(pages_, (std::vector<std::string>{"new_chat", "contact_detail"}));

  controller_.MessageContact("c-ann");

  EXPECT_EQ(opened_, std::vector<std::string>{"c-ann"});
  EXPECT_TRUE(pages_.empty()) << "nothing is left over the chat";
  EXPECT_EQ(tab_, NavTab::Sessions);
  EXPECT_EQ(pane_, "chat");
}

TEST_F(ContactCardMessageTest, MessageOnTheCardAlsoClosesAnAddContactPageUnderIt) {
  controller_.OpenAddContactPage();
  controller_.OnSelectContact("c-ann");
  ASSERT_EQ(pages_, (std::vector<std::string>{"add_contact", "contact_detail"}));

  controller_.MessageContact("c-ann");

  EXPECT_TRUE(pages_.empty());
  EXPECT_FALSE(controller_.AddContactPageOpen());
}

} // namespace
} // namespace pbr
