#include "foundation/i18n/ScriptLanguageDetector.h"

#include <gtest/gtest.h>

TEST(ScriptLanguageDetectorTest, DetectsJapanese) {
  EXPECT_EQ(pbr::DetectChatMessageLanguage("こんにちは"), "ja");
}

TEST(ScriptLanguageDetectorTest, DetectsKorean) {
  EXPECT_EQ(pbr::DetectChatMessageLanguage("안녕하세요"), "ko");
}

TEST(ScriptLanguageDetectorTest, DetectsTraditionalChinese) {
  EXPECT_EQ(pbr::DetectChatMessageLanguage("臺灣"), "zh-Hant");
}

TEST(ScriptLanguageDetectorTest, DetectsSimplifiedChinese) {
  EXPECT_EQ(pbr::DetectChatMessageLanguage("你好"), "zh-Hans");
}

TEST(ScriptLanguageDetectorTest, LatinOnlyHasNoLanguage) {
  EXPECT_FALSE(pbr::DetectChatMessageLanguage("Hello world").has_value());
}

TEST(ScriptLanguageDetectorTest, ApplyLangAttributeInsertsTag) {
  const std::string tag = pbr::ApplyLangAttribute(R"(<div class="bubble")", "你好");
  EXPECT_NE(tag.find(R"(lang="zh-Hans")"), std::string::npos);
}
