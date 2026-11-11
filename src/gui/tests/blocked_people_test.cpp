// Blocked people (Kenneth 2026-11-04): findable again to unblock — the list's contents, its markup and the
// Settings > Security row that opens it.
#include "gui/contacts/ContactsController.h"
#include "lib/people/ContactTypes.h"
#include "lib/ui/ViewCatalog.h"

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

Contact Person(const std::string& id, const std::string& name, const TrustLevel trust) {
  Contact contact;
  contact.id = id;
  contact.local.display_name = name;
  contact.local.trust = trust;
  SyncContactMirrors(contact);
  return contact;
}

TEST(BlockedPeopleTest, OnlyTheBlockedAreListedByName) {
  const std::vector<Contact> all = {
      Person("1", "Zoe", TrustLevel::Blocked),
      Person("2", "Amy", TrustLevel::Unknown),
      Person("3", "Bob", TrustLevel::Blocked),
      Person("4", "Cid", TrustLevel::Friendly),
  };
  const std::vector<Contact> blocked = BlockedContactsByName(all);
  ASSERT_EQ(blocked.size(), 2u);
  EXPECT_EQ(blocked[0].id, "3");
  EXPECT_EQ(blocked[1].id, "1");
  EXPECT_TRUE(BlockedContactsByName({Person("5", "Eve", TrustLevel::Unknown)}).empty());
}

TEST(BlockedPeopleTest, EachRowUnblocksInPlaceAndOpensThePersonOnTap) {
  EXPECT_EQ(ViewCatalog::ResolvePath("blocked_people"), "views/blocked_people.rml");
  const std::string rml = ReadAsset("views/blocked_people.rml");
  EXPECT_NE(rml.find("data-for=\"row : blocked_rows\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"blocked_unblock(row.id)\""), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"blocked_open_contact(row.id)\""), std::string::npos);
  // The result is the bottom pill, not an inline line.
  EXPECT_EQ(rml.find("blocked_status"), std::string::npos);
  EXPECT_NE(rml.find("{{i18n:contacts.blocked.empty}}"), std::string::npos);
  // The Unblock tap must not also open the person: it is not inside the element that opens them.
  const size_t unblock = rml.find("blocked_unblock(row.id)");
  const size_t name = rml.find("class=\"blocked-row-name\"");
  ASSERT_NE(name, std::string::npos);
  EXPECT_NE(rml.find("</p>", name), std::string::npos);
  EXPECT_LT(rml.find("</p>", name), unblock);
}

TEST(BlockedPeopleTest, PrivacyHasARowThatOpensTheList) {
  const std::string rml = ReadAsset("views/settings_section_privacy.rml");
  const size_t row = rml.find("data-event-click=\"open_blocked_people()\"");
  ASSERT_NE(row, std::string::npos);
  EXPECT_NE(rml.find("{{i18n:contacts.blocked.title}}", row), std::string::npos);
  EXPECT_NE(rml.find("data-if=\"blocked_count > 0\"", row), std::string::npos);
}

TEST(BlockedPeopleTest, TheUnblockButtonIsABigEnoughTarget) {
  const std::string css = ReadAsset("themes/components.rcss");
  const size_t button = css.find(".blocked-unblock {");
  ASSERT_NE(button, std::string::npos);
  EXPECT_NE(css.find("height: 44dp;", button), std::string::npos);
  const size_t row = css.find(".blocked-row {");
  ASSERT_NE(row, std::string::npos);
  EXPECT_NE(css.find("height: 60dp;", row), std::string::npos);
}

} // namespace
} // namespace pbr
