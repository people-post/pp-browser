#pragma once

#include <string>

namespace pbr {

struct LlmConfig {
  std::string api_key;
  std::string base_url = "https://www.brief.global/api/llm/v1";
  std::string model = "xai";
  std::string preset = "brief";
  bool require_api_key = true;
  int num_predict = 8192;
};

} // namespace pbr
