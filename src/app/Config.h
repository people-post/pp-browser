#pragma once

#include "agent/LlmClient.h"
#include "common/Error.h"
#include "common/Module.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct McpConfig {
  std::string command;
  std::vector<std::string> args;
};

struct SearchConfig {
  std::string provider = "duckduckgo";
  std::string api_key;
};

struct AppConfig {
  LlmConfig llm;
  std::string theme = "assets/themes/base.rcss";
  std::optional<McpConfig> mcp;
  SearchConfig search;
};

class Config : public Module {
public:
  static Config& Instance();

  static Roe<AppConfig> Load(int argc, char** argv);
  static Roe<AppConfig> LoadFromFile(const std::string& path);
  static AppConfig DefaultOllama();

private:
  Config();
};

} // namespace pbr
