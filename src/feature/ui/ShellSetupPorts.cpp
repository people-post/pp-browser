#include "feature/ui/ShellSetupPorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellSetupPorts MakeShellSetupPorts(ShellHost& shell) {
  ShellSetupPorts ports;
  ports.initialize = [&shell](Rml::Context* context) { shell.Initialize(context); };
  ports.fonts_ready = [&shell]() -> bool& { return shell.State().fonts_ready; };
  ports.register_pane = [&shell](const PaneSpec& spec) { shell.RegisterPane(spec); };
  ports.update = [&shell](Rml::Context* context) { shell.Update(context); };
  ports.sync_layout = [&shell]() { shell.SyncLayout(); };
  return ports;
}

} // namespace pbr
