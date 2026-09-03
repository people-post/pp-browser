#pragma once

#include "foundation/data/Config.h"

#include <string>

namespace pbr {

std::string ResolvePreset(const AppConfig& config);

/** Wire model id for a known preset (`brief` → grok-…, `ollama` → llama3.2, else empty). */
std::string DefaultModelForPreset(const std::string& preset_id);

/**
 * Apply preset URL / auth flags. Does not resolve model — use NormalizeLlmConfig for that.
 * @param custom_base_url used only when preset_id is `custom` (or unknown).
 */
void ApplyPreset(AppConfig& config, const std::string& preset_id, const std::string& custom_base_url);

/**
 * Single write-boundary normalizer: preset → base_url / require_api_key / model / inline keys.
 * Call after settings drafts and on config load so disk and runtime see precise values.
 * Runtime Brief vault key injection stays outside this (identity.enc).
 */
void NormalizeLlmConfig(AppConfig& config);

/** Sets require_api_key from preset and whether credentials are configured. */
void ResolveLlmAuthRequirements(AppConfig& config);

} // namespace pbr
