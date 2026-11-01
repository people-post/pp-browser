#pragma once

#include "lib/ai/ToolRegistry.h"
#include "feature/settings/SettingsCommands.h"

namespace pbr {

/**
 * Ports needed by settings agent tools. Same lifetime rules as SettingsCommands —
 * Application fills std::function captures; tools copy the functions (not this struct).
 */
struct SettingsToolPorts {
  std::function<ProfileIdentityView()> load_profile_identity;
  std::function<std::string(const std::string& language_pref)> language_display_label;
  std::function<std::vector<LocaleInfo>()> available_locales;
  std::function<void(const std::string& appearance_pref)> apply_appearance;
  std::function<SessionStore&()> session_store;
  std::function<bool()> messaging_ready;
  std::function<std::string()> last_mesh_error;
  std::function<SettingsReachabilityView()> load_reachability;
  std::function<PinProtectionView()> load_pin_protection;
  std::function<void(bool try_upnp)> run_reachability_probe;
  /**
   * Ensure the PP Support contact and open its chat (SettingsCommands::open_support_chat). Tools run on the worker
   * pool, so the app wires a version that runs on the UI thread and waits; not filled by SettingsToolPortsFromCommands.
   */
  std::function<Roe<void>()> open_support_chat;
};

/** Me-tab / Govern tools (prefs, status, safe config) as an MCP-shaped provider. */
class SettingsToolProvider : public IToolProvider {
public:
  explicit SettingsToolProvider(SettingsToolPorts ports);

  std::string Id() const override;
  std::vector<ToolDescriptor> ListTools() override;

private:
  SettingsToolPorts ports_;
};

void RegisterSettingsTools(ToolRegistry& registry, SettingsToolPorts ports);

/** Build tool ports from the same SettingsCommands Application binds to Me UI. */
SettingsToolPorts SettingsToolPortsFromCommands(const SettingsCommands& commands);

} // namespace pbr
