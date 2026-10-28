#include "feature/conversations/ContactActionDispatcher.h"

#include <gtest/gtest.h>

#include <string>

using namespace pbr;

namespace {

// A people list stores no keys: Add / Message name the account, and the dispatcher looks it up first.
TEST(ContactActionDispatcherTest, AddAndMessageByAccountLookUpTheDirectory) {
  EXPECT_EQ(DirectoryAccountToLookUp(R"({"type":"add_contact","account_id":"account:a","hit_id":"h"})"),
            std::optional<std::string>("account:a"));
  EXPECT_EQ(DirectoryAccountToLookUp(R"({"type":"start_conversation","account_id":"account:a"})"),
            std::optional<std::string>("account:a"));
}

TEST(ContactActionDispatcherTest, KnownContactsAndOtherActionsNeedNoLookup) {
  EXPECT_FALSE(DirectoryAccountToLookUp(R"({"type":"start_conversation","contact_id":"c1","account_id":"account:a"})"));
  EXPECT_FALSE(DirectoryAccountToLookUp(R"({"type":"show_contact","contact_id":"c1","account_id":"account:a"})"));
  EXPECT_FALSE(DirectoryAccountToLookUp(R"({"type":"add_contact"})"));
  EXPECT_FALSE(DirectoryAccountToLookUp(R"({"type":"add_contact","account_id":""})"));
  EXPECT_FALSE(DirectoryAccountToLookUp("not json"));
}

} // namespace
