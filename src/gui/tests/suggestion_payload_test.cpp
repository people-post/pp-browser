#include "gui/chat/SuggestionPayload.h"

#include <gtest/gtest.h>

using namespace pbr;

// The directory refuses an empty query (HTTP 400): the "find someone" chip opens Add contact and sends no tool call.
TEST(SuggestionPayloadTest, FindSomeoneRunsNoToolItOpensAddContact) {
  EXPECT_FALSE(SuggestionPayload("find_someone", "en").has_value());
}

TEST(SuggestionPayloadTest, ArticlesFollowTheUiLanguage) {
  for (const char* zh : {"zh-Hans", "zh", "zh-Hant"}) {
    const auto doc = TryParseObject(*SuggestionPayload("articles", zh));
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->getString("tool").value_or(""), "blog_articles");
    EXPECT_EQ(doc->getString("brf_domain").value_or(""), "cn");
    EXPECT_EQ(doc->getString("brf_language").value_or(""), "cn");
    EXPECT_EQ(doc->getIf<int64_t>("size").value_or(0), 10);
  }
  for (const char* other : {"en", "", "fr"}) {
    const auto doc = TryParseObject(*SuggestionPayload("articles", other));
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc->getString("brf_domain").value_or(""), "en");
    EXPECT_EQ(doc->getString("brf_language").value_or(""), "en");
  }
}

TEST(SuggestionPayloadTest, OtherChipsHaveNoPayload) {
  EXPECT_FALSE(SuggestionPayload("get_started", "en").has_value());
  EXPECT_FALSE(SuggestionPayload("", "en").has_value());
}
