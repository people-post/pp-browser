// The contact star ("常用联系人", Kenneth 2026-11-04): the contact page's star button stars and unstars the person; the
// choice is kept in this profile's preferences.json only and puts them first among the frequent contacts.
#include "gui/contacts/ContactsController.h"

#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "foundation/data/UserPreferences.h"
#include "lib/people/ContactTypes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
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

class ContactStarTest : public ::testing::Test {
protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("pp_contact_star_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "profile");
    BootstrapResult bootstrap{};
    bootstrap.config = Config::DefaultAppConfig();
    bootstrap.config_path = (root_ / "config.json").string();
    bootstrap.data_dir = root_.string();
    bootstrap.profile_data_dir = (root_ / "profile").string();
    bootstrap.profile_prefs = UserPreferences::DefaultProfile();
    store_.SetUiDemo(false);
    store_.Initialize(std::move(bootstrap));

    for (const char* id : {"c-ann", "c-bob"}) {
      Contact contact;
      contact.id = id;
      contact.display_name = id;
      contacts_[id] = contact;
    }
    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      const auto it = contacts_.find(id);
      return it == contacts_.end() ? std::optional<Contact>{} : std::optional<Contact>{it->second};
    };
    ports.list_contacts = [this]() -> Roe<std::vector<Contact>> {
      std::vector<Contact> all;
      for (const auto& [id, contact] : contacts_) {
        all.push_back(contact);
      }
      return all;
    };
    controller_.BindContactsPorts(ports);
    controller_.BindSessionStore(&store_);
  }

  void TearDown() override {
    controller_.BindSessionStore(nullptr);
    std::filesystem::remove_all(root_);
  }

  std::filesystem::path root_;
  SessionStore store_;
  std::map<std::string, Contact> contacts_;
  ContactsController controller_;
};

TEST_F(ContactStarTest, TheStarButtonStarsAndUnstarsAndTheChoiceIsSavedOnThisDevice) {
  controller_.OnSelectContact("c-bob");
  EXPECT_FALSE(controller_.SelectedStarred());

  controller_.OnToggleStar();
  EXPECT_TRUE(controller_.SelectedStarred());
  EXPECT_EQ(controller_.Starred(), std::vector<std::string>{"c-bob"});
  auto saved = UserPreferences::LoadProfile((root_ / "profile").string());
  ASSERT_TRUE(static_cast<bool>(saved));
  EXPECT_EQ(saved->starred_contacts, std::vector<std::string>{"c-bob"}) << "written to preferences.json";

  controller_.OnSelectContact("c-ann");
  EXPECT_FALSE(controller_.SelectedStarred());
  controller_.OnToggleStar();
  EXPECT_EQ(controller_.Starred(), (std::vector<std::string>{"c-bob", "c-ann"})) << "kept in star order";

  controller_.OnSelectContact("c-bob");
  EXPECT_TRUE(controller_.SelectedStarred()) << "a page opened again shows the star";
  controller_.OnToggleStar();
  EXPECT_FALSE(controller_.SelectedStarred());
  EXPECT_EQ(controller_.Starred(), std::vector<std::string>{"c-ann"});
}

TEST(ContactStarMarkupTest, ThePageHasAStarButtonAndTheListAStarAfterTheName) {
  const std::string page = ReadAsset("views/contact_detail.rml");
  EXPECT_NE(page.find("data-event-click=\"toggle_star()\""), std::string::npos);
  EXPECT_NE(page.find("{{i18n:contacts.star}}"), std::string::npos);
  EXPECT_NE(page.find("{{i18n:contacts.starred}}"), std::string::npos);
  const size_t star = page.find("toggle_star()");
  const size_t button = page.rfind("<button class=\"cd-act\"", star);
  ASSERT_NE(button, std::string::npos) << "a 76x64dp round-row button, like Message";
  EXPECT_LT(star - button, 80u);

  const std::string list = ReadAsset("views/contacts.rml");
  EXPECT_NE(list.find("class=\"contacts-row-star\""), std::string::npos);
  EXPECT_NE(list.find("data-if=\"contact.starred\""), std::string::npos);
}

} // namespace
} // namespace pbr
