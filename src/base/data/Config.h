#pragma once

#include "base/ai/LlmClient.h"
#include "base/ai/conversation/ConversationTypes.h"
#include "common/Error.h"
#include "common/Module.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct McpConfig {
  std::string command;
  std::vector<std::string> args;
  std::string url;

  bool IsConfigured() const { return !command.empty() || !url.empty(); }
};

struct ServiceEndpointConfig {
  std::string base_url;
};

struct SearchConfig {
  std::string provider = "duckduckgo";
  std::string api_key;
};

struct AppConfig {
  LlmConfig llm;
  std::string llm_api_key_env;
  ContextBudget context = DefaultContextBudget();
  std::string theme = "themes/base.rcss";
  std::string data_dir;
  ServiceEndpointConfig relay;
  ServiceEndpointConfig directory;
  ServiceEndpointConfig registration;
  std::optional<McpConfig> mcp;
  SearchConfig search;
};

class Config : public Module {
public:
  static Config& Instance();

  static constexpr int kConfigVersion = 1;

  static Roe<AppConfig> Load(int argc, char** argv);
  static Roe<AppConfig> LoadFromFile(const std::string& path);
  static AppConfig DefaultAppConfig();
  static Roe<void> SaveToFile(const std::string& path, const AppConfig& config);
  static std::string DiscoverConfigPath(int argc, char** argv);

  // Backward-compatible alias used by Application.h default arg.
  static AppConfig DefaultOllama() { return DefaultAppConfig(); }

private:
  Config();
};

} // namespace pbr
