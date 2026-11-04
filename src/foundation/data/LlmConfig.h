#pragma once

#include "common/Secret.h"

#include <string>

namespace pbr {

struct LlmConfig {
  SecretText api_key;
  std::string base_url;
  std::string model = "xai";
  std::string preset = "brief";
  bool require_api_key = true;
  int num_predict = 8192;
};

} // namespace pbr
