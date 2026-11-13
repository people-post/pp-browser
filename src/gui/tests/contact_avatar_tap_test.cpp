// Tapping a person's photo on their page opens it large (the avatar viewer); a letter avatar does nothing.
#include "gui/contacts/ContactsController.h"

#include "lib/people/ContactTypes.h"

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

class ContactAvatarTapTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (const char* id : {"c-photo", "c-letter"}) {
      Contact contact;
      contact.id = id;
      contact.display_name = id;
      contacts_.push_back(contact);
    }
    MessagingContactsPorts ports;
    ports.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      return view;
    };
    ports.get_contact = [this](const std::string& id) -> Roe<std::optional<Contact>> {
      for (const Contact& contact : contacts_) {
        if (contact.id == id) {
          return std::optional<Contact>{contact};
        }
      }
      return std::optional<Contact>{};
    };
    ports.list_contacts = [this]() -> Roe<std::vector<Contact>> { return contacts_; };
    ports.contact_icon_local_path = [](const Contact& contact) {
      return contact.id == "c-photo" ? std::string("/tmp/photo.jpg") : std::string();
    };
    controller_.BindContactsPorts(ports);
    controller_.BindAvatarViewer([this](const std::string& src) { opened_.push_back(src); });
  }

  std::vector<Contact> contacts_;
  std::vector<std::string> opened_;
  ContactsController controller_;
};

TEST_F(ContactAvatarTapTest, ThePhotoOpensInTheViewer) {
  controller_.OnSelectContact("c-photo");
  ASSERT_TRUE(controller_.Selected().has_icon);
  controller_.OnTapAvatar();
  EXPECT_EQ(opened_, std::vector<std::string>{"/tmp/photo.jpg"});
}

TEST_F(ContactAvatarTapTest, ALetterAvatarDoesNothing) {
  controller_.OnSelectContact("c-letter");
  ASSERT_FALSE(controller_.Selected().has_icon);
  controller_.OnTapAvatar();
  EXPECT_TRUE(opened_.empty());
}

TEST_F(ContactAvatarTapTest, AGroupMemberWithAPhotoOpensItToo) {
  controller_.OnShowMember("acct-1", "Zed", "/tmp/member.jpg");
  controller_.OnTapAvatar();
  EXPECT_EQ(opened_, std::vector<std::string>{"/tmp/member.jpg"});
}

TEST(ContactAvatarTapMarkupTest, TheAvatarFrameCallsTheHandlerInTheContactsModel) {
  const std::string page = ReadAsset("views/contact_detail.rml");
  const size_t tap = page.find("data-event-click=\"tap_contact_avatar()\"");
  ASSERT_NE(tap, std::string::npos);
  const size_t frame = page.rfind("<div class=\"contact-detail-avatar-frame\"", tap);
  ASSERT_NE(frame, std::string::npos);
  EXPECT_LT(tap - frame, 80u) << "on the avatar frame itself";
  EXPECT_NE(page.find("data-model=\"contacts\""), std::string::npos);
}

} // namespace
} // namespace pbr
