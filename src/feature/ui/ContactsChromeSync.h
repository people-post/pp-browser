#pragma once

#include "feature/ui/ContactsSurfaceSnapshot.h"
#include "feature/ui/ShellChromeOp.h"

namespace pbr {

/**
 * Shell-facing projection derived from ContactsSurfaceSnapshot.
 * Only these fields affect window chrome.
 */
struct ContactsShellProjection {
  bool detail_open = false;
  int contacts_unread = 0;
};

/** Surface snapshot → shell projection (no ShellHost). */
ContactsShellProjection ProjectContactsShell(const ContactsSurfaceSnapshot& surface);

/** Pure gate: structural detail → SyncLayout; unread → DirtyNav. */
ShellChromeOp ClassifyContactsChromeUpdate(const ContactsShellProjection& synced,
                                           const ContactsShellProjection& next);

} // namespace pbr
