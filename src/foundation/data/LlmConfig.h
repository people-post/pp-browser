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
  /**
   * Runtime only, never saved: the brief preset's `api_key` came from the account's registered slot
   * (`brief_llm_api_key`), not the guest slot. It decides the registered user's limits (MaxConcurrentTurnsFor), not
   * the key's text (RC018 item 2).
   */
  bool brief_key_registered = false;
};

} // namespace pbr
