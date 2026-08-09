#include "feature/ai/tools/WebSearchProvider.h"

#include "feature/ai/tools/WebSearchTool.h"

namespace pbr {

WebSearchProvider::WebSearchProvider(SearchConfig config) : config_(std::move(config)) {}

std::string WebSearchProvider::Id() const {
  return "web_search";
}

std::vector<ToolDescriptor> WebSearchProvider::ListTools() {
  ToolDescriptor tool = WebSearchTool::Make(config_);
  tool.meta.provider = Id();
  tool.meta.domain = "knowledge";
  tool.meta.risk = "read";
  tool.meta.mutating = false;
  return {std::move(tool)};
}

} // namespace pbr
