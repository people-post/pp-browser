#pragma once

#include <string>

namespace ppbrowser {

struct LlmConfig {
  std::string api_key;
  std::string base_url = "https://api.openai.com/v1";
  std::string model = "gpt-4o-mini";
  bool require_api_key = true;
};

class LlmClient {
public:
  explicit LlmClient(LlmConfig config);

  std::string Complete(const std::string& system_prompt, const std::string& user_prompt) const;

private:
  LlmConfig config_;
};

} // namespace ppbrowser
