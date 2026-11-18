// A person added on a phone is in the New chat list the moment the add completes, with the "Added {name}" pill, and no
// page has to be left and opened again. The real New chat and Add contact pages over the controller's real data model;
// the taps are the pages' own events; the scan's card answer comes later from another thread, as on the device.
#include "common/runtime/AppRuntime.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"
#include "gui/contacts/ContactsTickGate.h"
#include "gui/shell/DataModelHost.h"
#include "lib/messaging/ContactLocator.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class QuietSystem : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return 0.0; }
};

/** A page as the shell mounts it, its {{i18n:…}} words left out (the app's own text view fills them). */
std::string View(const std::string& name) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / name);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return std::regex_replace(buffer.str(), std::regex(R"(\{\{i18n:[^}]*\}\})"), "");
}

constexpr const char* kContext = "contact-add-list-render-test";

class ContactAddListRenderTest : public ::testing::Test {
protected:
  void SetUp() override {
    ContactsController::InstallInstance(controller_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render_);
    ui::SetSystemInterface(&system_);
    ASSERT_TRUE(ui::Initialise());
    context_ = ui::CreateContext(kContext, {390, 844});
    ASSERT_NE(context_, nullptr);

    ShellNavigationPorts nav;
    nav.snapshot = [this]() {
      ShellChromeSnapshot chrome;
      chrome.layout_mode = LayoutMode::Compact;
      chrome.nav_tab = tab_;
      chrome.contact_detail_transient = !pages_.empty() && pages_.back() == "contact_detail";
      return chrome;
    };
    nav.push_transient = [this](const PaneSpec& spec) { pages_.push_back(spec.key); };
    nav.pop_transient = [this]() {
      if (!pages_.empty()) {
        pages_.pop_back();
      }
    };
    nav.push_layer = [](const PaneSpec&) { return 1; };
    nav.select_nav_tab = [this](const NavTab tab) {
      tab_ = tab;
      pages_.clear(); // a tab change takes the pushed pages away (ShellHost::ClearTabContext)
    };
    controller_.BindShellNavigation(nav);

    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { toasts_.push_back(message); };
    feedback.show_confirm = [this](const std::string&, const std::string&, std::function<void(bool)> on_result,
                                   const std::string&) { confirm_ = std::move(on_result); };
    controller_.BindShellFeedback(feedback);
    controller_.BindScanPorts({.scan_qr = [this](QrScanResultCallback done) { decoded_ = std::move(done); }});

    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    store_.push_back(MakeContact("c-ann", "Ann"));
    ports.list_contacts = [this]() -> Roe<std::vector<Contact>> {
      std::lock_guard lock(mutex_);
      return store_;
    };
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      std::lock_guard lock(mutex_);
      for (const Contact& contact : store_) {
        if (contact.id == id) {
          return std::optional<Contact>{contact};
        }
      }
      return std::optional<Contact>{};
    };
    ports.search_people = [](const std::string&) -> Roe<std::vector<mesh::DirectoryHit>> {
      return std::vector<mesh::DirectoryHit>{BeaHit()};
    };
    ports.add_contact_from_directory_hit = [this](const mesh::DirectoryHit&) -> Roe<Contact> { return StoreBea(); };
    // As MessagingContactsPorts does: the contact is stored and reported on this thread; its card is asked of the
    // person's devices and relays and answers seconds later, on another thread.
    ports.add_from_locator = [this](const std::string&, std::function<void(Roe<std::string>)> added,
                                    std::function<void(bool)> card) {
      added(StoreBea().id);
      card_ = std::move(card);
    };
    controller_.BindContactsPorts(ports);
    controller_.OnNavTabActivated(); // a phone

    ASSERT_TRUE(controller_.RegisterModel(context_));
    document_ = context_->LoadDocumentFromMemory("<rml><head></head><body>" + View("new_chat.rml") +
                                                 View("add_contact.rml") + "</body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
  }
  void TearDown() override {
    controller_.OnAddContactDismissed();
    controller_.OnNewChatDismissed();
    controller_.BindContactsPorts({});
    controller_.BindShellNavigation({});
    controller_.BindShellFeedback({});
    controller_.BindScanPorts({});
    DataModelHost::Instance().Clear();
    ui::RemoveContext(kContext);
    ui::Shutdown();
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
    AppRuntime::Shutdown();
    ContactsController::ClearInstance();
  }

  static Contact MakeContact(const std::string& id, const std::string& name) {
    Contact contact;
    contact.id = id;
    contact.display_name = name;
    contact.remote.nickname = name;
    contact.remote.ids = {{mesh::ContactIdKind::Account, "account:" + id, true}};
    SyncContactMirrors(contact);
    return contact;
  }
  static mesh::DirectoryHit BeaHit() {
    mesh::DirectoryHit hit;
    hit.hit_id = "account:c-bea";
    hit.account_id = "account:c-bea";
    hit.display_name = "Bea";
    hit.nickname = "Bea";
    hit.ids = {{mesh::ContactIdKind::Account, "account:c-bea", true}};
    return hit;
  }
  Contact StoreBea() {
    std::lock_guard lock(mutex_);
    store_.push_back(MakeContact("c-bea", "Bea"));
    return store_.back();
  }

  /** The names the New chat page's rows show, as drawn. */
  std::vector<std::string> NewChatRows() {
    context_->Update();
    ui::ElementList rows;
    document_->QuerySelectorAll(rows, ".nc-page .contacts-row .contacts-row-title");
    std::vector<std::string> names;
    for (ui::Element* row : rows) {
      names.emplace_back(row->GetInnerRML().c_str());
    }
    return names;
  }
  bool NewChatShows(const std::string& name) {
    const std::vector<std::string> rows = NewChatRows();
    return std::find(rows.begin(), rows.end(), name) != rows.end();
  }
  void Tap(const std::string& selector, const size_t index = 0) {
    context_->Update();
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    ASSERT_LT(index, found.size()) << selector;
    found[index]->DispatchEvent(ui::EventId::Click, {});
  }
  /** What the main loop does each frame, for `ms` of wall time. */
  void Frames(const int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      if (ShouldTickContacts(tab_, controller_.DetailSheetOpen(), controller_.AddContactPageOpen())) {
        controller_.Tick();
      }
      AppRuntime::RunUITasks();
      context_->Update();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  /** Chats → New chat → Add contact, by the pages' own taps. */
  void OpenAddContactFromNewChat() {
    controller_.OpenNewChatPage(); // the Chats "+"
    ASSERT_EQ(NewChatRows(), std::vector<std::string>{"Ann"});
    Tap(".nc-page .nc-action", 1); // "Add contact"
    ASSERT_EQ(pages_, (std::vector<std::string>{"new_chat", "add_contact"})) << "Add contact opens over New chat";
  }

  NullRender render_;
  QuietSystem system_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ContactsController controller_;
  std::mutex mutex_;
  std::vector<Contact> store_;
  std::vector<std::string> pages_; // the shell's pushed pages, top last
  NavTab tab_ = NavTab::Sessions;
  std::vector<std::string> toasts_;
  std::function<void(bool)> confirm_;
  QrScanResultCallback decoded_;
  std::function<void(bool)> card_;
};

TEST_F(ContactAddListRenderTest, AScannedPersonIsInTheNewChatListWithThePillBeforeTheirCardComes) {
  OpenAddContactFromNewChat();
  Tap(".ac-page .ac-top-card", 0); // "Scan"
  ASSERT_TRUE(decoded_);
  ASSERT_EQ(pages_, std::vector<std::string>{"new_chat"}) << "the scanner opens over New chat";
  decoded_(EncodeContactLocator({.account_id = "account:c-bea", .nickname = "Bea"}));
  ASSERT_TRUE(confirm_) << "the app asks whether to add this person";
  confirm_(true);

  // Their card has not come yet (card_ is still waiting): the add is done all the same.
  ASSERT_TRUE(card_);
  EXPECT_EQ(tab_, NavTab::Sessions) << "no tab change took the New chat page away";
  EXPECT_EQ(toasts_, std::vector<std::string>{"Added Bea"});
  EXPECT_EQ(pages_, (std::vector<std::string>{"new_chat", "contact_detail"})) << "their page, over New chat";
  EXPECT_TRUE(NewChatShows("Bea")) << "the list under their page already has them";

  // Seconds later their card answers, from another thread: no second pill, the list stays right.
  std::thread answer([card = card_]() { card(true); });
  answer.join();
  Frames(30);
  EXPECT_EQ(toasts_, std::vector<std::string>{"Added Bea"});
  EXPECT_EQ(NewChatRows(), (std::vector<std::string>{"Ann", "Bea"}));

  pages_.pop_back(); // back from their page
  EXPECT_EQ(pages_, std::vector<std::string>{"new_chat"});
  EXPECT_TRUE(NewChatShows("Bea"));
}

TEST_F(ContactAddListRenderTest, APersonFoundByIdIsInTheNewChatListWithThePill) {
  OpenAddContactFromNewChat();
  controller_.TypeInAddContact("bea"); // what the search field's change event does
  // The 300 ms wait, then the directory on a worker, its answer posted back to the UI thread.
  for (int i = 0; i < 100 && controller_.AddSearching(); ++i) {
    Frames(30);
  }
  ASSERT_FALSE(controller_.AddSearching());
  Tap(".ac-page .ac-results .contacts-row", 0); // the result
  if (confirm_) {
    confirm_(true); // "Add Bea?" (a name, not an exact ID)
  }

  EXPECT_EQ(toasts_, std::vector<std::string>{"Added Bea"});
  EXPECT_EQ(pages_, (std::vector<std::string>{"new_chat", "contact_detail"}));
  EXPECT_TRUE(NewChatShows("Bea"));
  pages_.pop_back();
  EXPECT_TRUE(NewChatShows("Bea"));
}

TEST_F(ContactAddListRenderTest, APersonAddedElsewhereShowsOnceTheAddressBookSaysSo) {
  controller_.OpenNewChatPage();
  ASSERT_FALSE(NewChatShows("Bea"));
  StoreBea(); // the chat menu's Add, an AI tool, a shared card: not this controller
  controller_.OnContactsChanged();
  EXPECT_TRUE(NewChatShows("Bea"));
}

} // namespace
} // namespace pbr
