#pragma once

#include "foundation/data/Config.h"
#include "feature/settings/SettingsUiState.h"

#include <string>
#include <vector>

namespace pbr {

struct SettingsDraft {
  std::string llm_preset;
  std::string llm_base_url;
  std::string llm_model;
  std::string llm_api_key;
  std::string llm_api_key_env;
};

AppConfig ApplyLlmSettingsDraft(const AppConfig& base, const SettingsDraft& draft);
AppConfig ApplyIntegrationsSettingsDraft(const AppConfig& base, const SettingsUiState& state);
/**
 * A custom backend's relay or registration endpoint as its field shows it (multi-relay MR008): its relay link,
 * `base_url#peer_id`, or the base URL alone when it names no PeerId.
 */
std::string EndpointLinkText(const ServiceEndpointConfig& endpoint);
/** The field's text back into `endpoint`: a relay link sets the base URL and the PeerId; a bare URL clears the PeerId. */
void SetEndpointFromLinkText(ServiceEndpointConfig& endpoint, const std::string& text);

AppConfig ApplyNetworkSettingsDraft(const AppConfig& base, const SettingsUiState& state);

} // namespace pbr
