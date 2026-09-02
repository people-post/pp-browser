#pragma once

#include "base/ai/ToolRegistry.h"
#include "foundation/data/Config.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr {

class WebSearchTool {
public:
  static ToolDescriptor Make(const SearchConfig& config);
  static Roe<std::string> Search(const SearchConfig& config, const Object& arguments);

  // Exposed for unit tests.
  static Value ParseDuckDuckGoInstantAnswerJson(const std::string& json_text);
  static Value ParseDuckDuckGoLiteHtmlResults(const std::string& html);
  static Value ParseDuckDuckGoHtmlPageResults(const std::string& html);
  static Value ParseGoogleNewsRssItems(const std::string& xml);
};

} // namespace pbr
