#include "feature/presenters/GroupEventText.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/PskRotateCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// System lines stored in English before show in the UI language (the stored row is not rewritten).

class LegacySystemLineTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_P(LegacySystemLineTest, EveryStoredEnglishLineReadsInTheLanguageOfTheUi) {
  const bool zh = std::string(GetParam()) == "zh-Hans";
  for (const char* stored :
       {"Accepted group invitation", "Declined group invitation", "You joined Book club", "You declined Book club",
        "You blocked the inviter", "Noted — you can keep chatting with people who are online.",
        "New messages stay on this device.", "Chat key updated.", "Owner hasn’t been reachable\nYou can keep chatting",
        kPskRotateLockText, kPskRotateRekeyText}) {
    const auto text = LegacySystemLineText(stored);
    ASSERT_TRUE(text) << stored;
    EXPECT_FALSE(text->empty()) << stored;
    EXPECT_EQ(text->find("chat."), std::string::npos) << "a missing key answers with itself: " << *text;
    if (zh) {
      EXPECT_NE(*text, stored) << "still the stored English";
    }
  }
}

TEST_P(LegacySystemLineTest, TheGroupNameIsKept) {
  const auto joined = LegacySystemLineText("You joined Book club");
  ASSERT_TRUE(joined);
  EXPECT_NE(joined->find("Book club"), std::string::npos) << *joined;
  const auto declined = LegacySystemLineText("You declined Book club");
  ASSERT_TRUE(declined);
  EXPECT_NE(declined->find("Book club"), std::string::npos) << *declined;
}

TEST_P(LegacySystemLineTest, OtherTextIsLeftAlone) {
  EXPECT_FALSE(LegacySystemLineText("See you at 8"));
  EXPECT_FALSE(LegacySystemLineText(""));
  EXPECT_FALSE(LegacySystemLineText("You joined")) << "no title: not the legacy line";
  EXPECT_FALSE(LegacySystemLineText("You joined the group")) << "a group event, worded from its type";
}

INSTANTIATE_TEST_SUITE_P(Locales, LegacySystemLineTest, ::testing::Values("en", "zh-Hans"));

} // namespace
} // namespace pbr
