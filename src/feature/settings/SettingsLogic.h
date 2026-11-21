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
 * `base_url#relay:…`, or the base URL alone when it names no relay id.
 */
std::string EndpointLinkText(const ServiceEndpointConfig& endpoint);
/** The field's text back into `endpoint`: a relay link sets the base URL and the relay id; a bare URL clears it. */
void SetEndpointFromLinkText(ServiceEndpointConfig& endpoint, const std::string& text);

AppConfig ApplyNetworkSettingsDraft(const AppConfig& base, const SettingsUiState& state);
/**
 * The setting a release build misses to use the backend's relay (multi-relay MR008 item 4): `relay.relay_id`, or
 * `registration.relay_id` when registration is another origin; "" when nothing is missing — a developer build, the
 * compiled backend (`compiled_backend`: the build names it), or both named. Me → Network shows it; the hub does not use
 * such a relay.
 */
std::string MissingRelayIdSetting(const AppConfig& config, bool developer_build, bool compiled_backend);

} // namespace pbr
