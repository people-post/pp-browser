#include "feature/ai/ToolRegistry.h"

#include "feature/ai/IToolProvider.h"

#include <sstream>

namespace pbr {

namespace {

std::string FormatMetaTag(const ToolMeta& meta) {
  std::string tag;
  if (!meta.domain.empty()) {
    tag = meta.domain;
  }
  if (!meta.risk.empty()) {
    if (!tag.empty()) {
      tag += ", ";
    }
    tag += meta.risk;
  } else if (meta.mutating) {
    if (!tag.empty()) {
      tag += ", ";
    }
    tag += "write";
  }
  return tag;
}

} // namespace

ToolRegistry::ToolRegistry() {
  redirectLogger("ToolRegistry");
}

void ToolRegistry::Register(ToolDescriptor tool) {
  for (const ToolDescriptor& existing : tools_) {
    if (existing.definition.name == tool.definition.name) {
      log().warning << "Replacing duplicate tool: " << tool.definition.name;
    }
  }
  tools_.push_back(std::move(tool));
}

void ToolRegistry::RegisterProvider(IToolProvider& provider) {
  const std::string provider_id = provider.Id();
  for (ToolDescriptor& tool : provider.ListTools()) {
    if (tool.meta.provider.empty()) {
      tool.meta.provider = provider_id;
    }
    bool exists = false;
    for (const ToolDescriptor& existing : tools_) {
      if (existing.definition.name == tool.definition.name) {
        log().warning << "Skipping duplicate tool from provider " << provider_id << ": "
                      << tool.definition.name;
        exists = true;
        break;
      }
    }
    if (exists) {
      continue;
    }
    tools_.push_back(std::move(tool));
  }
}

void ToolRegistry::Clear() {
  tools_.clear();
}

std::vector<ToolDefinition> ToolRegistry::Definitions() const {
  std::vector<ToolDefinition> out;
  out.reserve(tools_.size());
  for (const ToolDescriptor& tool : tools_) {
    out.push_back(tool.definition);
  }
  return out;
}

std::string ToolRegistry::SummaryForPrompt() const {
  std::ostringstream out;
  for (const ToolDescriptor& tool : tools_) {
    out << "- " << tool.definition.name;
    const std::string tag = FormatMetaTag(tool.meta);
    if (!tag.empty()) {
      out << " [" << tag << "]";
    }
    out << ": " << tool.definition.description << "\n";
  }
  return out.str();
}

Roe<std::string> ToolRegistry::Execute(const std::string& name, const nlohmann::json& arguments) const {
  for (const ToolDescriptor& tool : tools_) {
    if (tool.definition.name == name) {
      return tool.execute(arguments);
    }
  }
  return Error("Unknown tool: " + name);
}

} // namespace pbr
