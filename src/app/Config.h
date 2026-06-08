#pragma once

#include "agent/LlmClient.h"

#include <optional>
#include <string>

namespace ppbrowser {

struct AppConfig {
  LlmConfig llm;
  std::string theme = "assets/themes/base.rcss";
};

class Config {
public:
  static std::optional<AppConfig> LoadFromFile(const std::string& path);
  static AppConfig DefaultOllama();
};

} // namespace ppbrowser
