#pragma once

#include "domain/ui/ShellTypes.h"

#include <functional>

namespace Rml {
class Context;
}

namespace pbr {

/**
 * Shell bootstrap ports for ChatController::Setup (pane registration, initial layout).
 * Application fills from ShellHost. Clear via BindShellSetup({}).
 *
 * Ports must not return mutable ShellHost::State references.
 */
struct ShellSetupPorts {
  std::function<void(Rml::Context* context)> initialize;
  std::function<void(bool)> set_fonts_ready;
  std::function<bool()> fonts_ready;
  std::function<void(const PaneSpec& spec)> register_pane;
  std::function<void(Rml::Context* context)> update;
  std::function<void()> sync_layout;
};

class ShellHost;

ShellSetupPorts MakeShellSetupPorts(ShellHost& shell);

} // namespace pbr
