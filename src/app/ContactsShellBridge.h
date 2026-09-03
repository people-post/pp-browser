#pragma once

#include "gui/contacts/ContactsChromeSync.h"
#include "gui/contacts/ContactsSurfaceSnapshot.h"
#include "gui/shell/ShellChromeApplyPorts.h"

namespace pbr {

class ContactsShellBridge {
public:
  void BindApply(ShellChromeApplyPorts ports);
  void Clear();

  void OnSurface(const ContactsSurfaceSnapshot& surface);
  /** Last pushed surface (for BadgeAggregator contacts_unread). */
  const ContactsSurfaceSnapshot& LastSurface() const { return last_surface_; }

private:
  ContactsSurfaceSnapshot last_surface_{};
  ContactsShellProjection synced_{};
  ShellChromeApplyPorts apply_ports_;
};

} // namespace pbr
