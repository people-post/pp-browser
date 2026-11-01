#include "foundation/data/LlmPreset.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

class ScopedOrigin {
public:
  explicit ScopedOrigin(const char* value) {
    if (const char* old = std::getenv("PP_BROWSER_ORIGIN")) {
      had_ = true;
      old_ = old;
    }
    set(value);
  }
  ~ScopedOrigin() { set(had_ ? old_.c_str() : nullptr); }

private:
  static void set(const char* value) {
#ifdef _WIN32
    _putenv_s("PP_BROWSER_ORIGIN", value ? value : "");
#else
    if (value) {
      setenv("PP_BROWSER_ORIGIN", value, 1);
    } else {
      unsetenv("PP_BROWSER_ORIGIN");
    }
#endif
  }
  bool had_ = false;
  std::string old_;
};

constexpr const char* kOrigin = "https://backend.example.test";

} // namespace

TEST(LlmPresetTest, DefaultModelForKnownPresets) {
  EXPECT_EQ(pbr::DefaultModelForPreset("brief"), "xai");
  EXPECT_EQ(pbr::DefaultModelForPreset("cloud"), "gpt-4o-mini");
  EXPECT_EQ(pbr::DefaultModelForPreset("ollama"), "llama3.2");
  EXPECT_TRUE(pbr::DefaultModelForPreset("custom").empty());
}

TEST(LlmPresetTest, NormalizeReplacesPresetNameAsModel) {
  ScopedOrigin origin(kOrigin);
  pbr::AppConfig config;
  config.llm.preset = "brief";
  config.llm.model = "brief";
  config.llm.api_key = "should-clear";
  pbr::NormalizeLlmConfig(config);
  EXPECT_EQ(config.llm.model, "xai");
  EXPECT_EQ(config.llm.base_url, "https://backend.example.test/api/llm/v1");
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

TEST(LlmPresetTest, BriefPresetWithoutOriginHasEmptyBaseUrl) {
  ScopedOrigin origin(nullptr);
  if (std::string(pbr::CompiledDefaultOrigin()).empty()) {
    pbr::AppConfig config;
    config.llm.preset = "brief";
    pbr::NormalizeLlmConfig(config);
    EXPECT_TRUE(config.llm.base_url.empty());
    EXPECT_TRUE(pbr::BriefLlmServiceBaseUrl(config).empty());
  }
}
