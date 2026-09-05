#include "gui/shell/ShellSetupPorts.h"

#include "gui/shell/ShellHost.h"

namespace pbr {

ShellSetupPorts MakeShellSetupPorts(ShellHost& shell) {
  ShellSetupPorts ports;
  ports.initialize = [&shell](Rml::Context* context) { shell.Initialize(context); };
  ports.set_fonts_ready = [&shell](const bool ready) { shell.State().fonts_ready = ready; };
  ports.fonts_ready = [&shell]() { return shell.State().fonts_ready; };
  ports.register_pane = [&shell](const PaneSpec& spec) { shell.RegisterPane(spec); };
  ports.update = [&shell](Rml::Context* context) { shell.Update(context); };
  ports.sync_layout = [&shell]() { shell.SyncLayout(); };
  return ports;
}

} // namespace pbr
