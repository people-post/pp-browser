#pragma once

#include "feature/ui/ContactsSurfaceSnapshot.h"

namespace pbr {

/**
 * Shell-facing projection derived from ContactsSurfaceSnapshot.
 * Only these fields affect window chrome.
 */
struct ContactsShellProjection {
  bool detail_open = false;
  int contacts_unread = 0;
};

enum class ContactsChromeUpdate {
  None,      // projection unchanged
  DirtyNav,  // unread / nav-relevant binding change
  SyncLayout // detail open/close structural chrome
};

/** Surface snapshot → shell projection (no ShellHost). */
ContactsShellProjection ProjectContactsShell(const ContactsSurfaceSnapshot& surface);

/** Pure gate: structural detail → SyncLayout; unread → DirtyNav. */
ContactsChromeUpdate ClassifyContactsChromeUpdate(const ContactsShellProjection& synced,
                                                  const ContactsShellProjection& next);

} // namespace pbr
