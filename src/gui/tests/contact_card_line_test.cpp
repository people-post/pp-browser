// A contact's card says so ("In your contacts") under the name; a person who is not a contact yet (a member preview)
// has the explicit Add contact button instead, and a blocked contact keeps its Blocked badge only.
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadAsset(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / relative);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

TEST(ContactCardLineTest, TheLineShowsOnlyForARealUnblockedContact) {
  const std::string rml = ReadAsset("views/contact_detail.rml");
  const size_t at = rml.find("cd-is-contact");
  ASSERT_NE(at, std::string::npos);
  const std::string tag = rml.substr(rml.rfind('<', at), rml.find("</p>", at) - rml.rfind('<', at));
  EXPECT_NE(tag.find("!selected.member_preview && selected.trust_key != 'blocked'"), std::string::npos);
  EXPECT_NE(tag.find("{{i18n:contacts.is_contact}}"), std::string::npos);
  // The non-contact preview keeps its primary Add contact action.
  EXPECT_NE(rml.find("add_member_contact()"), std::string::npos);
}

TEST(ContactCardLineTest, TheLineIsTranslated) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("contacts.is_contact"), "已是联系人");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
