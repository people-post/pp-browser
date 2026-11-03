// Phone Contacts: the "+" menu entries, the short account id and the avatar colour shared with the Chats list.
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/people/ContactTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace pbr {
namespace {

class ContactsPhoneTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  }
  void SetUp() override { LocalizationService::Instance().SetPreferredLanguage("en"); }

  static std::vector<std::string> Ids(const std::vector<ContextMenuAction>& actions) {
    std::vector<std::string> ids;
    for (const ContextMenuAction& action : actions) {
      ids.push_back(action.id);
    }
    return ids;
  }
};

TEST_F(ContactsPhoneTest, PhoneMenuHasAddScanGroupAndFindInOrder) {
  const auto ids = Ids(ContactsController::AddContactMenuActions(true, true, true));
  EXPECT_EQ(ids, (std::vector<std::string>{"add_contact", "scan_qr", "new_group", "find_someone"}));
}

TEST_F(ContactsPhoneTest, PhoneMenuOmitsEntriesThatAreNotWired) {
  EXPECT_EQ(Ids(ContactsController::AddContactMenuActions(true, false, false)),
            (std::vector<std::string>{"add_contact", "find_someone"}));
}

TEST_F(ContactsPhoneTest, DesktopMenuKeepsAddAndFindOnly) {
  EXPECT_EQ(Ids(ContactsController::AddContactMenuActions(false, true, true)),
            (std::vector<std::string>{"add_contact", "find_someone"}));
}

// Kenneth 2026-11-03: "联系人这一页在右边加入一个字母排序吧。中文的用按拼音排序，英文的按字母排序".
TEST_F(ContactsPhoneTest, ContactsAreSectionedByLetterChineseByPinyinHashLast) {
  std::vector<ContactsController::ContactListRow> rows;
  for (const char* title : {"张伟", "Emma", "123 Studio", "李雷", "alice", "王芳", "阿明", "Zoe"}) {
    ContactsController::ContactListRow row;
    row.title = title;
    rows.push_back(row);
  }
  const auto sections = ContactsController::ContactSections(rows);
  std::vector<std::string> letters;
  std::vector<std::string> titles;
  for (const auto& section : sections) {
    letters.push_back(section.letter.c_str());
    for (const auto& row : section.rows) {
      titles.push_back(row.title.c_str());
    }
  }
  EXPECT_EQ(letters, (std::vector<std::string>{"A", "E", "L", "W", "Z", "#"}));
  EXPECT_EQ(titles, (std::vector<std::string>{"alice", "阿明", "Emma", "李雷", "王芳", "Zoe", "张伟", "123 Studio"}));

  const auto index = ContactsController::ContactIndexLetters(sections);
  ASSERT_EQ(index.size(), 27u);
  EXPECT_EQ(std::string(index.front().letter.c_str()), "A");
  EXPECT_EQ(std::string(index.back().letter.c_str()), "#");
  EXPECT_TRUE(index[0].present);   // A
  EXPECT_FALSE(index[1].present);  // B
  EXPECT_TRUE(index[26].present);  // #
}

TEST_F(ContactsPhoneTest, AnIndexLetterWithoutContactsJumpsToTheNextSection) {
  std::vector<ContactsController::NewChatSectionRow> sections = {{"A", {}}, {"L", {}}, {"#", {}}};
  EXPECT_EQ(ContactsController::ContactSectionForLetter(sections, 'A'), 0);
  EXPECT_EQ(ContactsController::ContactSectionForLetter(sections, 'B'), 1);
  EXPECT_EQ(ContactsController::ContactSectionForLetter(sections, 'L'), 1);
  EXPECT_EQ(ContactsController::ContactSectionForLetter(sections, 'M'), 2);
  EXPECT_EQ(ContactsController::ContactSectionForLetter(sections, '#'), 2);
  sections.pop_back();
  EXPECT_EQ(ContactsController::ContactSectionForLetter(sections, 'Z'), 1);  // past the last: the last
  EXPECT_EQ(ContactsController::ContactSectionForLetter({}, 'A'), -1);
}

TEST_F(ContactsPhoneTest, DetailShowsOnlyTheLastSixCharactersAndColoursByName) {
  Contact contact;
  contact.id = "c1";
  contact.display_name = "Ada";
  contact.remote.ids = {{mesh::ContactIdKind::Account, "account:0123456789abcdef", true}};
  SyncContactMirrors(contact);
  const ContactsController::ContactDetail detail = ToContactDetail(contact, {});
  EXPECT_EQ(std::string(detail.short_id.c_str()), "abcdef");
  // The same input as the Chats list (the title), so one person has one colour in both tabs.
  EXPECT_EQ(detail.avatar_tone, MakeAvatarGlyph(detail.title.c_str(), {}).tone);
  EXPECT_EQ(std::string(detail.avatar_letter.c_str()), MakeAvatarGlyph(detail.title.c_str(), {}).letter);
}

// Kenneth 2026-11-03: a tap on a group member who is not a contact opens their page in its member state.
TEST_F(ContactsPhoneTest, AMemberWhoIsNotAContactGetsAMemberPage) {
  const ContactsController::ContactDetail detail =
      MemberPreviewDetail("account:0123456789abcdef", "Grace", "/tmp/grace.png");
  EXPECT_TRUE(detail.member_preview);
  EXPECT_TRUE(detail.id.empty()) << "not a contact: no contact id, so nothing edits or removes a contact";
  EXPECT_EQ(std::string(detail.member_account.c_str()), "account:0123456789abcdef");
  EXPECT_EQ(std::string(detail.title.c_str()), "Grace");
  EXPECT_EQ(std::string(detail.short_id.c_str()), "abcdef");
  EXPECT_TRUE(detail.has_icon);
  EXPECT_FALSE(detail.can_message);
  EXPECT_EQ(detail.avatar_tone, MakeAvatarGlyph("Grace", {}).tone);
  EXPECT_FALSE(MemberPreviewDetail("account:x", "", "").title.empty()) << "an unnamed member still has a title";
  EXPECT_FALSE(MemberPreviewDetail("account:x", "", "").has_icon);
}

} // namespace
} // namespace pbr
