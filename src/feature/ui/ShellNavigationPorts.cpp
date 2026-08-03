#include "feature/ui/ShellNavigationPorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellNavigationPorts MakeShellNavigationPorts(ShellHost& shell) {
  ShellNavigationPorts navigation;
  navigation.snapshot = [&shell]() { return ProjectShellChromeSnapshot(shell.State()); };
  navigation.clear_local_back = [&shell](const std::string& id) { shell.ClearLocalBack(id); };
  navigation.push_local_back = [&shell](const std::string& id, std::function<void()> commit) {
    shell.PushLocalBack(id, std::move(commit));
  };
  navigation.has_local_back = [&shell](const std::string& id) { return shell.HasLocalBack(id); };
  navigation.select_nav_tab = [&shell](const NavTab tab) { shell.SelectNavTab(tab); };
  navigation.open_account_sheet = [&shell]() { shell.OpenAccountSheet(); };
  navigation.close_account_sheet = [&shell]() { shell.CloseAccountSheet(); };
  navigation.clear_primary_pane = [&shell]() { shell.ClearPrimaryPane(); };
  navigation.set_primary_pane = [&shell](const std::string& key) { shell.SetPrimaryPane(key); };
  navigation.push_transient = [&shell](const PaneSpec& spec) { shell.PushTransient(spec); };
  navigation.pop_transient = [&shell]() { shell.PopTransient(); };
  navigation.open_compact_chat = [&shell]() { shell.OpenCompactChat(); };
  navigation.close_compact_chat = [&shell]() { shell.CloseCompactChat(); };
  navigation.request_dismiss_instant = [&shell]() { return shell.RequestDismiss(DismissStyle::Instant); };
  navigation.refresh_dismiss_gestures = [&shell]() { shell.RefreshDismissGestures(); };
  navigation.request_remount_nav_rail = [&shell]() { shell.RequestRemountNavRail(); };
  navigation.set_activity = [&shell](const bool visible, const Rml::String& message) {
    shell.SetActivity(visible, message);
  };
  navigation.request_sync_layout = [&shell](const bool restore, const char* reason) {
    shell.RequestSyncLayout(restore, reason);
  };
  navigation.fonts_ready = [&shell]() -> bool& { return shell.State().fonts_ready; };
  navigation.set_nav_badges = [&shell](const NavBadgeState& badges) {
    shell.State().nav_badges = badges;
    shell.DirtyNavChrome();
  };
  navigation.set_auxiliary_available = [&shell](const bool available) { shell.SetAuxiliaryAvailable(available); };
  navigation.open_auxiliary = [&shell]() { shell.OpenAuxiliary(); };
  navigation.close_auxiliary = [&shell]() { shell.CloseAuxiliary(); };
  navigation.push_layer = [&shell](const PaneSpec& spec) { return shell.PushLayer(spec); };
  navigation.close_layer = [&shell](const int layer_id) { shell.CloseLayer(layer_id); };
  return navigation;
}

} // namespace pbr
