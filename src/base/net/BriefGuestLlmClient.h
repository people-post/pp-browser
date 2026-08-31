#pragma once

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

struct BriefGuestLlmMintResult {
  std::string llm_api_key;
  std::string expires_at;
};

/**
 * POST {base_url}/guest/start — mints a free-tier Brief Bearer (brf_guest_*).
 * base_url should be BriefLlmBaseUrl() (…/api/llm/v1) without trailing slash preference handled here.
 */
Roe<BriefGuestLlmMintResult> MintBriefGuestLlmKey(const std::string& llm_base_url,
                                                 const std::string& install_id = {});

/** Prefer registered brf_llm_* over guest brf_guest_*. */
std::string ResolveBriefLlmApiKey(const std::string& registered_key, const std::string& guest_key);

} // namespace pbr
