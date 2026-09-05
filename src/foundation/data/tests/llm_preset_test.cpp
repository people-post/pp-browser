#include "foundation/data/LlmPreset.h"

#include <gtest/gtest.h>

TEST(LlmPresetTest, DefaultModelForKnownPresets) {
  EXPECT_EQ(pbr::DefaultModelForPreset("brief"), "xai");
  EXPECT_EQ(pbr::DefaultModelForPreset("cloud"), "gpt-4o-mini");
  EXPECT_EQ(pbr::DefaultModelForPreset("ollama"), "llama3.2");
  EXPECT_TRUE(pbr::DefaultModelForPreset("custom").empty());
}

TEST(LlmPresetTest, NormalizeReplacesPresetNameAsModel) {
  pbr::AppConfig config;
  config.llm.preset = "brief";
  config.llm.model = "brief";
  config.llm.api_key = "should-clear";
  pbr::NormalizeLlmConfig(config);
  EXPECT_EQ(config.llm.model, "xai");
  EXPECT_EQ(config.llm.base_url, "https://www.brief.global/api/llm/v1");
  EXPECT_TRUE(config.llm.api_key.empty());
  EXPECT_TRUE(config.llm.require_api_key);
}

TEST(LlmPresetTest, NormalizeKeepsExplicitModelOverride) {
  pbr::AppConfig config;
  config.llm.preset = "brief";
  config.llm.model = "grok-custom-override";
  pbr::NormalizeLlmConfig(config);
  EXPECT_EQ(config.llm.model, "grok-custom-override");
}

TEST(LlmPresetTest, NormalizeFillsEmptyModel) {
  pbr::AppConfig config;
  config.llm.preset = "ollama";
  config.llm.model.clear();
  pbr::NormalizeLlmConfig(config);
  EXPECT_EQ(config.llm.model, "llama3.2");
  EXPECT_EQ(config.llm.base_url, "http://localhost:11434/v1");
}
