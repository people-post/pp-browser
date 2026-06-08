#pragma once

#include "agent/LlmClient.h"
#include "common/Error.h"
#include "common/Module.h"

#include <string>

namespace pbr {

struct AppConfig {
  LlmConfig llm;
  std::string theme = "assets/themes/base.rcss";
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
