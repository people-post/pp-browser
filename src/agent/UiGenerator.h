#pragma once

#include "agent/LlmClient.h"
#include "bindings/BindingsManifest.h"

#include <string>

namespace ppbrowser {

struct GeneratedUi {
  std::string rml;
  std::string rcss;
  std::string bindings_json;
};

class UiGenerator {
public:
  UiGenerator(LlmClient& llm, std::string rml_profile);

  GeneratedUi Generate(const std::string& tools_context);

  static bool ExtractBlocks(const std::string& llm_output, GeneratedUi& out);

private:
  LlmClient& llm_;
  std::string rml_profile_;
};

} // namespace ppbrowser
