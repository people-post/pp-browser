#include "feature/ui/ContactsChromeSync.h"

namespace pbr {

ContactsShellProjection ProjectContactsShell(const ContactsSurfaceSnapshot& surface) {
  ContactsShellProjection projection;
  projection.detail_open = surface.detail_open;
  projection.contacts_unread = surface.contacts_unread < 0 ? 0 : surface.contacts_unread;
  return projection;
}

ContactsChromeUpdate ClassifyContactsChromeUpdate(const ContactsShellProjection& synced,
                                                  const ContactsShellProjection& next) {
  if (synced.detail_open != next.detail_open) {
    return ContactsChromeUpdate::SyncLayout;
  }
  if (synced.contacts_unread != next.contacts_unread) {
    return ContactsChromeUpdate::DirtyNav;
  }
  return ContactsChromeUpdate::None;
}

} // namespace pbr
