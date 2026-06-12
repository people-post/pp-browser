#pragma once

#include "agent/ToolRegistry.h"
#include "app/Config.h"
#include "common/Error.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pbr {

class WebSearchTool {
public:
  static ToolDescriptor Make(const SearchConfig& config);
  static Roe<std::string> Search(const SearchConfig& config, const nlohmann::json& arguments);
};

} // namespace pbr
