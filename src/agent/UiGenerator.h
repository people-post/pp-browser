#pragma once

#include "agent/LlmClient.h"
#include "bindings/BindingsManifest.h"
#include "common/Error.h"
#include "common/Module.h"

#include <string>

namespace pbr {

struct GeneratedUi {
  std::string rml;
  std::string rcss;
  std::string bindings_json;
};

class UiGenerator : public Module {
public:
  UiGenerator(LlmClient& llm, std::string rml_profile);

  Roe<GeneratedUi> Generate(const std::string& tools_context);

  static bool ExtractBlocks(const std::string& llm_output, GeneratedUi& out);

private:
  LlmClient& llm_;
  std::string rml_profile_;
};

} // namespace pbr
