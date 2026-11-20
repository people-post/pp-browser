#pragma once

#include "common/Secret.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct BriefGuestLlmMintResult {
  SecretText llm_api_key;
  std::string expires_at;
};

/**
 * POST {base_url}/guest/start — mints a free-tier Brief Bearer (the guest slot's key).
 * base_url should be BriefLlmBaseUrl() (…/api/llm/v1) without trailing slash preference handled here.
 */
Roe<BriefGuestLlmMintResult> MintBriefGuestLlmKey(const std::string& llm_base_url, const std::string& install_id = {});

/** The Bearer to use: the registered slot's token over the guest slot's key (by slot, never by the key's text). */
std::string ResolveBriefLlmApiKey(const std::string& registered_key, const std::string& guest_key);

} // namespace pbr
