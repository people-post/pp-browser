#include "agent/UiGenerator.h"

#include "agent/PromptBuilder.h"
#include "agent/RmlValidator.h"

#include <regex>
#include <stdexcept>

namespace pbr {

UiGenerator::UiGenerator(LlmClient& llm, std::string rml_profile)
    : llm_(llm), rml_profile_(std::move(rml_profile)) {
  redirectLogger("UiGenerator");
}

GeneratedUi UiGenerator::Generate(const std::string& tools_context) {
  const std::string system = PromptBuilder::BuildUiGenerationPrompt(tools_context, rml_profile_);
  const std::string user = "Generate a minimal form UI for the listed tools.";
  const std::string raw = llm_.Complete(system, user);

  GeneratedUi ui;
  if (!ExtractBlocks(raw, ui)) {
    log().error << "Failed to parse LLM UI blocks";
    throw std::runtime_error("Failed to parse LLM UI blocks");
  }

  auto rml_check = RmlValidator::ValidateRml(ui.rml);
  if (!rml_check.ok) {
    for (const auto& err : rml_check.errors) {
      log().error << "RML validation failed: " << err;
    }
    throw std::runtime_error("RML validation failed");
  }
  auto binding_check = RmlValidator::ValidateBindings(ui.bindings_json);
  if (!binding_check.ok) {
    for (const auto& err : binding_check.errors) {
      log().error << "Bindings validation failed: " << err;
    }
    throw std::runtime_error("Bindings validation failed");
  }
  log().info << "UI generation succeeded";
  return ui;
}

bool UiGenerator::ExtractBlocks(const std::string& llm_output, GeneratedUi& out) {
  static const std::regex rml_re(R"re(```rml\s*([\s\S]*?)```)re", std::regex::icase);
  static const std::regex rcss_re(R"re(```rcss\s*([\s\S]*?)```)re", std::regex::icase);
  static const std::regex json_re(R"re(```json\s*([\s\S]*?)```)re", std::regex::icase);

  std::smatch match;
  if (!std::regex_search(llm_output, match, rml_re)) {
    return false;
  }
  out.rml = match[1].str();

  if (std::regex_search(llm_output, match, rcss_re)) {
    out.rcss = match[1].str();
  }

  if (!std::regex_search(llm_output, match, json_re)) {
    return false;
  }
  out.bindings_json = match[1].str();
  return true;
}

} // namespace pbr
