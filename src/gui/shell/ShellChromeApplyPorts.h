#pragma once

#include "gui/shell/ShellChromeOp.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * Apply shell chrome ops on ShellHost. Used by app-owned *ShellBridge translators,
 * not by presenters.
 */
struct ShellChromeApplyPorts {
  std::function<void(ShellChromeOp)> apply;
};

class ShellHost;

/** `sync_reason` is logged by RequestSyncLayout when op is SyncLayout. */
ShellChromeApplyPorts MakeShellChromeApplyPorts(ShellHost& shell, std::string sync_reason);

} // namespace pbr
