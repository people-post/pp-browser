#include "lib/ui/NewChatList.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

std::vector<std::string> Ids(const std::vector<NewChatContact>& contacts, const std::vector<size_t>& indexes) {
  std::vector<std::string> ids;
  for (const size_t i : indexes) {
    ids.push_back(contacts[i].id);
  }
  return ids;
}

NewChatContact C(const std::string& id, const std::string& name, const bool blocked = false) {
  return {id, name, blocked};
}

TEST(NewChatListTest, FrequentIsTheMostRecentDirectChatsNewestFirstAtMostFive) {
  std::vector<NewChatContact> contacts;
  std::vector<NewChatDirectChat> chats;
  for (int i = 0; i < 7; ++i) {
    contacts.push_back(C("c" + std::to_string(i), "Name" + std::to_string(i)));
    chats.push_back({"c" + std::to_string(i), 100 + i});
  }
  contacts.push_back(C("quiet", "Quiet")); // no chat
  const NewChatList list = BuildNewChatList(contacts, chats);
  EXPECT_EQ(Ids(contacts, list.frequent), (std::vector<std::string>{"c6", "c5", "c4", "c3", "c2"}));
}

TEST(NewChatListTest, FrequentSkipsBlockedAndChatsWithoutActivity) {
  const std::vector<NewChatContact> contacts = {C("a", "Ann"), C("b", "Bob", true), C("c", "Cy")};
  const std::vector<NewChatDirectChat> chats = {{"a", 10}, {"b", 99}, {"c", 0}, {"gone", 50}};
  const NewChatList list = BuildNewChatList(contacts, chats);
  EXPECT_EQ(Ids(contacts, list.frequent), (std::vector<std::string>{"a"}));
}

TEST(NewChatListTest, SeveralChatsWithOneContactCountTheNewest) {
  const std::vector<NewChatContact> contacts = {C("a", "Ann"), C("b", "Bob")};
  const std::vector<NewChatDirectChat> chats = {{"a", 10}, {"a", 90}, {"b", 50}};
  EXPECT_EQ(Ids(contacts, BuildNewChatList(contacts, chats).frequent), (std::vector<std::string>{"a", "b"}));
}

TEST(NewChatListTest, SectionsAreLettersAThroughZSortedByNameCaseBlind) {
  const std::vector<NewChatContact> contacts = {C("1", "bob"), C("2", "Alice"), C("3", "alan"), C("4", "Bella")};
  const NewChatList list = BuildNewChatList(contacts, {});
  ASSERT_EQ(list.sections.size(), 2u);
  EXPECT_EQ(list.sections[0].letter, "A");
  EXPECT_EQ(Ids(contacts, list.sections[0].contacts), (std::vector<std::string>{"3", "2"}));
  EXPECT_EQ(list.sections[1].letter, "B");
  EXPECT_EQ(Ids(contacts, list.sections[1].contacts), (std::vector<std::string>{"4", "1"}));
}

TEST(NewChatListTest, ChineseNamesGoUnderTheirPinyinInitialDigitsAndSymbolsUnderHashAtTheEnd) {
  const std::vector<NewChatContact> contacts = {C("1", "王五"), C("2", "Zed"), C("3", "7up"), C("4", "Amy"),
                                                C("5", "@home")};
  const NewChatList list = BuildNewChatList(contacts, {});
  ASSERT_EQ(list.sections.size(), 4u);
  EXPECT_EQ(list.sections[0].letter, "A");
  EXPECT_EQ(list.sections[1].letter, "W");
  EXPECT_EQ(list.sections[2].letter, "Z");
  EXPECT_EQ(list.sections[3].letter, "#");
  EXPECT_EQ(list.sections[3].contacts.size(), 2u);
}

TEST(NewChatListTest, BlockedContactsAreNotListed) {
  const std::vector<NewChatContact> contacts = {C("1", "Amy", true), C("2", "Ann")};
  const NewChatList list = BuildNewChatList(contacts, {{"1", 5}});
  EXPECT_TRUE(list.frequent.empty());
  ASSERT_EQ(list.sections.size(), 1u);
  EXPECT_EQ(Ids(contacts, list.sections[0].contacts), (std::vector<std::string>{"2"}));
}

TEST(NewChatListTest, AQueryFiltersByNameAndDropsTheFrequentGroup) {
  const std::vector<NewChatContact> contacts = {C("1", "Amy"), C("2", "Tammy"), C("3", "Bob"), C("4", "王小明")};
  const std::vector<NewChatDirectChat> chats = {{"1", 5}};
  NewChatList list = BuildNewChatList(contacts, chats, "MY");
  EXPECT_TRUE(list.frequent.empty());
  ASSERT_EQ(list.sections.size(), 2u);
  EXPECT_EQ(list.sections[0].letter, "A");
  EXPECT_EQ(list.sections[1].letter, "T");
  list = BuildNewChatList(contacts, chats, "小明");
  ASSERT_EQ(list.sections.size(), 1u);
  EXPECT_EQ(list.sections[0].letter, "W");
}

TEST(NewChatListTest, NoContactsGivesAnEmptyList) {
  const NewChatList list = BuildNewChatList({}, {});
  EXPECT_TRUE(list.frequent.empty());
  EXPECT_TRUE(list.sections.empty());
}

} // namespace
} // namespace pbr
