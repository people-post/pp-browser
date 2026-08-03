#pragma once

#include "feature/ui/ContactsChromeSync.h"

#include <functional>

namespace pbr {

/**
 * Apply contacts chrome updates on ShellHost. Used by ContactsShellBridge (app),
 * not by ContactsController.
 */
struct ShellContactsChromePorts {
  std::function<void(ContactsChromeUpdate)> apply_chrome_update;
};

class ShellHost;

ShellContactsChromePorts MakeShellContactsChromePorts(ShellHost& shell);

} // namespace pbr
