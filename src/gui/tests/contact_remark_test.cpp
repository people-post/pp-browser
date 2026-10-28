// The contact detail's remark field: wording, saving into the local annotation and its mirror, and the cap.
#include "common/ContactLimits.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/contacts/ContactsController.h"
#include "lib/people/ContactTypes.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

namespace pbr {
namespace {

class ContactRemarkTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  }

  void SetUp() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }

  static Contact Ada() {
    Contact contact;
    contact.id = "c1";
    contact.remote.nickname = "Ada";
    contact.remote.ids = {{mesh::ContactIdKind::Account, "account:ada", true}};
    SyncContactMirrors(contact);
    return contact;
  }
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TEST_F(ContactRemarkTest, DetailLabelsTheFieldAsARemarkAndShowsTheirNickname) {
  const ContactsController::ContactDetail detail = ToContactDetail(Ada(), {});
  EXPECT_EQ(std::string(detail.display_name.c_str()), "");
  EXPECT_EQ(Tr("contacts.display_name"), "Remark");
  EXPECT_EQ(std::string(detail.name_placeholder.c_str()), "Add a remark; only you can see it");
  EXPECT_EQ(std::string(detail.their_nickname.c_str()), "Their nickname: Ada");
  EXPECT_EQ(std::string(detail.title.c_str()), "Ada");

  // The view binds the same field and the same line.
  const std::string rml = ReadFile(std::string(PP_BROWSER_ASSETS_DIR) + "/views/contact_detail.rml");
  EXPECT_NE(rml.find("{{i18n:contacts.display_name}}"), std::string::npos);
  EXPECT_NE(rml.find("selected.their_nickname"), std::string::npos);
}

TEST_F(ContactRemarkTest, SavingTheRemarkUpdatesLocalAndMirrorAndClearingReturnsTheNickname) {
  const Contact original = Ada();
  ContactsController::ContactDetail detail = ToContactDetail(original, {});

  detail.display_name = "Alice-work";
  const Contact saved = BuildContactFromDetail(original, detail);
  EXPECT_EQ(saved.local.display_name, "Alice-work");
  EXPECT_EQ(saved.display_name, "Alice-work");
  EXPECT_EQ(ContactEffectiveTitle(saved), "Alice-work");
  EXPECT_EQ(saved.remote.nickname, "Ada");

  detail = ToContactDetail(saved, {});
  EXPECT_EQ(std::string(detail.display_name.c_str()), "Alice-work");
  EXPECT_EQ(std::string(detail.title.c_str()), "Alice-work");
  EXPECT_EQ(std::string(detail.their_nickname.c_str()), "Their nickname: Ada");

  detail.display_name = "";
  const Contact cleared = BuildContactFromDetail(saved, detail);
  EXPECT_TRUE(cleared.local.display_name.empty());
  EXPECT_TRUE(cleared.display_name.empty());
  EXPECT_EQ(ContactEffectiveTitle(cleared), "Ada");
}

TEST_F(ContactRemarkTest, RemarkIsCappedAtTheNicknameLimitInCodePoints) {
  ContactsController::ContactDetail detail = ToContactDetail(Ada(), {});
  std::string long_remark;
  for (size_t i = 0; i < kMaxNicknameChars + 8; ++i) {
    long_remark += "备";
  }
  detail.display_name = long_remark.c_str();
  const Contact saved = BuildContactFromDetail(Ada(), detail);
  EXPECT_EQ(saved.local.display_name.size(), kMaxNicknameChars * 3);
  EXPECT_EQ(saved.display_name, saved.local.display_name);
}

TEST_F(ContactRemarkTest, ChineseWording) {
  LocalizationService& l10n = LocalizationService::Instance();
  l10n.SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("contacts.display_name"), "备注");
  EXPECT_EQ(Tr("contacts.remark.placeholder"), "添加备注，只有你自己看得到");
  EXPECT_EQ(Tr("contacts.remark.their_nickname", {{"nickname", "艾丽"}}), "对方昵称：艾丽");
}

}  // namespace
}  // namespace pbr
