// Phone Contacts: the "+" menu entries, the short account id and the avatar colour shared with the Chats list.
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"
#include "lib/people/AvatarGlyph.h"
#include "lib/people/ContactTypes.h"
#include "lib/ui/ViewCatalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// The phone "+" opens the Add contact sheet; only the desktop keeps the menu.
TEST_F(ContactsPhoneTest, DesktopMenuKeepsAddAndFindOnly) {
  EXPECT_EQ(Ids(ContactsController::AddContactMenuActions()), (std::vector<std::string>{"add_contact", "find_someone"}));
}

// Kenneth 2026-11-04: the New group sheet was titled "新建对话".
TEST_F(ContactsPhoneTest, NewGroupSheetTitleSaysGroup) {
  EXPECT_EQ(Tr("people_picker.title"), "New group");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("people_picker.title"), "新建群");
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

// Kenneth 2026-11-04: Share > a chat showed "No card for this contact yet" (raw, English). The card is asked for
// once and the share tried again; only when none comes is a plain message shown.
TEST_F(ContactsPhoneTest, ShareRetriesOnceAfterTheCardArrives) {
  int shares = 0;
  int refreshes = 0;
  std::vector<bool> results;
  ContactsController::ShareContactCard(
      [&]() -> Roe<void> {
        ++shares;
        if (shares == 1) {
          return Error("No card for this contact yet");
        }
        return {};
      },
      [&](std::function<void(bool)> kept) {
        ++refreshes;
        kept(true);
      },
      [&](bool shared) { results.push_back(shared); });
  EXPECT_EQ(shares, 2);
  EXPECT_EQ(refreshes, 1);
  EXPECT_EQ(results, (std::vector<bool>{true}));
}

TEST_F(ContactsPhoneTest, ShareGivesUpWhenNoCardComes) {
  int shares = 0;
  std::vector<bool> results;
  ContactsController::ShareContactCard(
      [&]() -> Roe<void> {
        ++shares;
        return Error("No card for this contact yet");
      },
      [&](std::function<void(bool)> kept) { kept(false); },
      [&](bool shared) { results.push_back(shared); });
  EXPECT_EQ(shares, 1);
  EXPECT_EQ(results, (std::vector<bool>{false}));
}

TEST_F(ContactsPhoneTest, ShareThatWorksAsksForNothing) {
  int refreshes = 0;
  std::vector<bool> results;
  ContactsController::ShareContactCard([]() -> Roe<void> { return {}; },
                                       [&](std::function<void(bool)> kept) {
                                         ++refreshes;
                                         kept(true);
                                       },
                                       [&](bool shared) { results.push_back(shared); });
  EXPECT_EQ(refreshes, 0);
  EXPECT_EQ(results, (std::vector<bool>{true}));
}

TEST_F(ContactsPhoneTest, ShareWithoutARefreshPathFailsOnce) {
  std::vector<bool> results;
  ContactsController::ShareContactCard([]() -> Roe<void> { return Error("Messaging not ready"); }, nullptr,
                                       [&](bool shared) { results.push_back(shared); });
  EXPECT_EQ(results, (std::vector<bool>{false}));
}

TEST_F(ContactsPhoneTest, ShareFailureMessageIsLocalized) {
  EXPECT_NE(Tr("contacts.share.not_yet"), "contacts.share.not_yet");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  const std::string zh = Tr("contacts.share.not_yet");
  EXPECT_EQ(zh.find("card"), std::string::npos);
  EXPECT_NE(zh, "contacts.share.not_yet");
}


// Kenneth 2026-11-04: "我输一个字母，他就直接都筛选了" - the Contacts search matches name, remark, nickname and account ID.
Contact MakeContact(const std::string& display_name, const std::string& nickname, const std::string& account_id) {
  Contact contact;
  contact.id = "c-" + display_name;
  contact.local.display_name = display_name;
  contact.remote.nickname = nickname;
  mesh::ContactId id;
  id.kind = mesh::ContactIdKind::Account;
  id.value = account_id;
  id.primary = true;
  contact.remote.ids.push_back(id);
  SyncContactMirrors(contact);
  return contact;
}

TEST_F(ContactsPhoneTest, ContactSearchMatchesNameRemarkNicknameAndIdFragment) {
  const Contact zhang = MakeContact("张伟", "Wei Z", "account:AbC123xyz");
  const Contact bob = MakeContact("Bob Chen", "", "account:Q9q9q9");
  EXPECT_TRUE(ContactMatchesQuery(zhang, "")) << "an empty query shows everyone (clearing restores the list)";
  EXPECT_TRUE(ContactMatchesQuery(bob, ""));
  EXPECT_TRUE(ContactMatchesQuery(zhang, "张"));
  EXPECT_TRUE(ContactMatchesQuery(zhang, "wei z")) << "the nickname, case-blind";
  EXPECT_TRUE(ContactMatchesQuery(bob, "BOB"));
  EXPECT_TRUE(ContactMatchesQuery(bob, "chen"));
  EXPECT_TRUE(ContactMatchesQuery(zhang, "abc12")) << "a fragment of the account ID, case-blind";
  EXPECT_FALSE(ContactMatchesQuery(bob, "abc12"));
  EXPECT_FALSE(ContactMatchesQuery(bob, "zzz"));
}

TEST_F(ContactsPhoneTest, ContactSearchMatchesChineseNamesByPinyinInitials) {
  const Contact zhang = MakeContact("张伟", "", "account:a1");
  const Contact wang = MakeContact("王芳", "", "account:a2");
  const Contact zoe = MakeContact("Zoe", "", "account:a3");
  EXPECT_TRUE(ContactMatchesQuery(zhang, "zw"));
  EXPECT_TRUE(ContactMatchesQuery(zhang, "ZW"));
  EXPECT_FALSE(ContactMatchesQuery(wang, "zw"));
  EXPECT_TRUE(ContactMatchesQuery(zhang, "z")) << "z: everyone whose name starts with Z";
  EXPECT_TRUE(ContactMatchesQuery(zoe, "z"));
  EXPECT_FALSE(ContactMatchesQuery(wang, "z"));
  EXPECT_TRUE(ContactMatchesQuery(wang, "wf"));
}

// The Add contact sheet: every row and string the markup needs exists, in both languages.
std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST_F(ContactsPhoneTest, AddContactSheetHasSearchAccountCardAndFourRows) {
  EXPECT_EQ(ViewCatalog::ResolvePath("add_contact"), "views/add_contact.rml");
  EXPECT_EQ(ViewCatalog::ResolvePath("my_qr"), "views/my_qr.rml");
  const std::string rml = ReadAsset("views/add_contact.rml");
  ASSERT_FALSE(rml.empty());
  for (const char* piece :
       {"class=\"sheet-header\"", "class=\"sheet-title\"", "data-value=\"add_query\"",
        "data-event-change=\"add_search_changed()\"", "add_copy_id()", "add_share()", "add_action('scan')",
        "add_action('qr')", "add_action('invite')", "add_action('group')", "add_pick(hit.index)",
        "{{i18n:contacts.add_sheet.search_placeholder}}", "{{i18n:settings.profile.relay_id_none}}",
        "data-if=\"add_no_results\""}) {
    EXPECT_NE(rml.find(piece), std::string::npos) << piece;
  }
  // The account card and the rows are what an empty query shows; the buttons only exist with an account.
  EXPECT_LT(rml.find("add_copy_id()"), rml.find("add_action('scan')"));
  EXPECT_LT(rml.find("add_action('scan')"), rml.find("add_action('qr')"));
  EXPECT_LT(rml.find("add_action('qr')"), rml.find("add_action('invite')"));
  EXPECT_LT(rml.find("add_action('invite')"), rml.find("add_action('group')"));
  EXPECT_NE(rml.find("data-if=\"my_has_account\" data-event-click=\"add_copy_id()\""), std::string::npos);
}

TEST_F(ContactsPhoneTest, AddContactSheetStringsAreTranslated) {
  EXPECT_EQ(Tr("contacts.add_sheet.search_placeholder"), "Search by name or account ID");
  EXPECT_EQ(Tr("contacts.my_qr"), "My QR code");
  EXPECT_EQ(Tr("contacts.invite_friends"), "Invite friends to PP");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("contacts.add"), "添加联系人");
  EXPECT_EQ(Tr("contacts.add_sheet.search_placeholder"), "输入名字或账号 ID 找人");
  EXPECT_EQ(Tr("contacts.my_qr"), "我的二维码");
  EXPECT_EQ(Tr("contacts.invite_friends"), "邀请朋友使用 PP");
  EXPECT_EQ(Tr("scan.title"), "扫一扫");
  EXPECT_EQ(Tr("contacts.new_group"), "建群");
}

} // namespace
} // namespace pbr
