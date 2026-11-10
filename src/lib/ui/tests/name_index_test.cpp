// The phone Contacts index: a name's section letter (pinyin initial for Chinese) and the order inside a letter.
#include "lib/ui/NameIndex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(NameIndexTest, ChineseNamesGoUnderTheirPinyinInitial) {
  EXPECT_EQ(NameIndexLetter("张"), 'Z');
  EXPECT_EQ(NameIndexLetter("李"), 'L');
  EXPECT_EQ(NameIndexLetter("王"), 'W');
  EXPECT_EQ(NameIndexLetter("阿"), 'A');
  EXPECT_EQ(NameIndexLetter("张伟"), 'Z');
  EXPECT_EQ(NameIndexLetter("陈小明"), 'C');
}

TEST(NameIndexTest, LatinNamesGoUnderTheirFirstLetterCaseBlind) {
  EXPECT_EQ(NameIndexLetter("Emma"), 'E');
  EXPECT_EQ(NameIndexLetter("emma"), 'E');
  EXPECT_EQ(NameIndexLetter("  bob"), 'B');
}

TEST(NameIndexTest, DigitsSymbolsOtherScriptsAndEmptyGoUnderHash) {
  EXPECT_EQ(NameIndexLetter("123"), '#');
  EXPECT_EQ(NameIndexLetter("@home"), '#');
  EXPECT_EQ(NameIndexLetter("Ольга"), '#');
  EXPECT_EQ(NameIndexLetter("😀 Sun"), '#');
  EXPECT_EQ(NameIndexLetter(""), '#');
  EXPECT_EQ(NameIndexLetter("\xE5"), '#'); // a cut-off UTF-8 sequence
}

TEST(NameIndexTest, ContactsSortByLetterThenByInitialsWithHashLast) {
  std::vector<std::string> names = {"王小明", "123", "bob", "张三", "Alice", "王伟", "李雷", "阿明", "Zed", "#tag"};
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return NameIndexLess(a, b); });
  EXPECT_EQ(names,
            (std::vector<std::string>{"Alice", "阿明", "bob", "李雷", "王伟", "王小明", "Zed", "张三", "#tag", "123"}));
}

TEST(NameIndexTest, InitialsAreLatinLettersAndPinyinInitials) {
  EXPECT_EQ(NameInitials("张伟"), "zw");
  EXPECT_EQ(NameInitials("Bob Chen"), "bobchen");
  EXPECT_EQ(NameInitials("David 陈大卫"), "davidcdw");
  EXPECT_EQ(NameInitials("123 @"), "");
}

TEST(NameIndexTest, QueryMatchesTextCaseBlindOrByInitialsPrefix) {
  EXPECT_TRUE(TextMatchesQuery("张伟", ""));
  EXPECT_TRUE(TextMatchesQuery("张伟", "zw"));
  EXPECT_TRUE(TextMatchesQuery("张伟", "ZW"));
  EXPECT_TRUE(TextMatchesQuery("张伟", "z"));
  EXPECT_FALSE(TextMatchesQuery("张伟", "w")) << "initials match from the start";
  EXPECT_TRUE(TextMatchesQuery("张伟", "伟"));
  EXPECT_TRUE(TextMatchesQuery("Emma Johnson", "JOHN"));
  EXPECT_FALSE(TextMatchesQuery("Emma Johnson", "xyz"));
}

} // namespace
} // namespace pbr
