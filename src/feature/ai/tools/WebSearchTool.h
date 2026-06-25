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

  // Exposed for unit tests.
  static nlohmann::json ParseDuckDuckGoInstantAnswerJson(const std::string& json_text);
  static nlohmann::json ParseDuckDuckGoLiteHtmlResults(const std::string& html);
  static nlohmann::json ParseDuckDuckGoHtmlPageResults(const std::string& html);
  static nlohmann::json ParseGoogleNewsRssItems(const std::string& xml);
};

} // namespace pbr
