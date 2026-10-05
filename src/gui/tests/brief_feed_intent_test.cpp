#include "gui/chat/BriefFeedIntent.h"

#include <gtest/gtest.h>

using namespace pbr;

// "简报" / "briefs" typed in the AI composer shows the Brief feed, like the Home chip.
TEST(BriefFeedIntentTest, AsksForTheFeed) {
  for (const char* text : {"给我今天最新的简报", "最新简报", "今天的简报", "看看简报", "有什么新简报吗", "简报"}) {
    const BriefFeedRequest request = MatchBriefFeedRequest(text);
    EXPECT_TRUE(request.matched) << text;
    EXPECT_TRUE(request.chinese) << text;
  }
  for (const char* text : {"Show me the latest briefs", "new briefs", "Today's briefing", "any briefings today?", "Briefs"}) {
    const BriefFeedRequest request = MatchBriefFeedRequest(text);
    EXPECT_TRUE(request.matched) << text;
    EXPECT_FALSE(request.chinese) << text;
  }
}

// Writing or explaining a briefing, a briefing about a topic, and ordinary news questions go to the AI.
TEST(BriefFeedIntentTest, EverythingElseGoesToTheAi) {
  for (const char* text : {"帮我写一份简报", "关于伊朗的简报", "简报是什么", "把这段总结成简报", "给我今天最新的文章", "今天有什么新闻",
                           "Give me a brief summary of the news", "Be brief", "Write a briefing for my boss",
                           "A briefing on Iran", "What are briefs?", "Latest articles", ""}) {
    EXPECT_FALSE(MatchBriefFeedRequest(text).matched) << text;
  }
  EXPECT_FALSE(MatchBriefFeedRequest(std::string(200, 'x') + " briefs").matched); // not a short ask
}
