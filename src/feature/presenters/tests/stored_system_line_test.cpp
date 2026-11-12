#include "feature/presenters/GroupEventText.h"

#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// System lines their writers store in English show in the UI language (the stored row is not rewritten).

class StoredSystemLineTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_P(StoredSystemLineTest, EveryStoredEnglishLineReadsInTheLanguageOfTheUi) {
  const bool zh = std::string(GetParam()) == "zh-Hans";
  for (const char* stored :
       {"Accepted group invitation", "Declined group invitation", "You joined Book club", "You declined Book club",
        "You blocked the inviter", "Noted — you can keep chatting with people who are online.",
        "New messages stay on this device.", "Chat key updated.",
        "Owner hasn’t been reachable\nYou can keep chatting"}) {
    const auto text = StoredSystemLineText(stored);
    ASSERT_TRUE(text) << stored;
    EXPECT_FALSE(text->empty()) << stored;
    EXPECT_EQ(text->find("chat."), std::string::npos) << "a missing key answers with itself: " << *text;
    if (zh) {
      EXPECT_NE(*text, stored) << "still the stored English";
    }
  }
}

TEST_P(StoredSystemLineTest, TheGroupNameIsKept) {
  const auto joined = StoredSystemLineText("You joined Book club");
  ASSERT_TRUE(joined);
  EXPECT_NE(joined->find("Book club"), std::string::npos) << *joined;
  const auto declined = StoredSystemLineText("You declined Book club");
  ASSERT_TRUE(declined);
  EXPECT_NE(declined->find("Book club"), std::string::npos) << *declined;
}

TEST_P(StoredSystemLineTest, OtherTextIsLeftAlone) {
  EXPECT_FALSE(StoredSystemLineText("See you at 8"));
  EXPECT_FALSE(StoredSystemLineText(""));
  EXPECT_FALSE(StoredSystemLineText("You joined")) << "no title: not such a line";
  EXPECT_FALSE(StoredSystemLineText("You joined the group")) << "a group event, worded from its type";
}

INSTANTIATE_TEST_SUITE_P(Locales, StoredSystemLineTest, ::testing::Values("en", "zh-Hans"));

} // namespace
} // namespace pbr
