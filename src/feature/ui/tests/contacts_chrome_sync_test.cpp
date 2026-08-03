#include "feature/ui/ContactsChromeSync.h"

#include <gtest/gtest.h>

TEST(ContactsChromeSyncTest, UnchangedIsNone) {
  const pbr::ContactsSurfaceSnapshot surface = {.detail_open = false, .contacts_unread = 0};
  const pbr::ContactsShellProjection layer = pbr::ProjectContactsShell(surface);
  EXPECT_EQ(pbr::ClassifyContactsChromeUpdate(layer, layer), pbr::ShellChromeOp::None);
}

TEST(ContactsChromeSyncTest, DetailOpenSyncsLayout) {
  const pbr::ContactsShellProjection synced =
      pbr::ProjectContactsShell({.detail_open = false, .contacts_unread = 0});
  const pbr::ContactsShellProjection next =
      pbr::ProjectContactsShell({.detail_open = true, .contacts_unread = 0});
  EXPECT_EQ(pbr::ClassifyContactsChromeUpdate(synced, next), pbr::ShellChromeOp::SyncLayout);
}

TEST(ContactsChromeSyncTest, DetailCloseSyncsLayout) {
  const pbr::ContactsShellProjection synced =
      pbr::ProjectContactsShell({.detail_open = true, .contacts_unread = 2});
  const pbr::ContactsShellProjection next =
      pbr::ProjectContactsShell({.detail_open = false, .contacts_unread = 2});
  EXPECT_EQ(pbr::ClassifyContactsChromeUpdate(synced, next), pbr::ShellChromeOp::SyncLayout);
}

TEST(ContactsChromeSyncTest, UnreadChangeDirtiesNav) {
  const pbr::ContactsShellProjection synced =
      pbr::ProjectContactsShell({.detail_open = true, .contacts_unread = 0});
  const pbr::ContactsShellProjection next =
      pbr::ProjectContactsShell({.detail_open = true, .contacts_unread = 3});
  EXPECT_EQ(pbr::ClassifyContactsChromeUpdate(synced, next), pbr::ShellChromeOp::DirtyNav);
}

TEST(ContactsChromeSyncTest, DetailChangeWinsOverUnread) {
  const pbr::ContactsShellProjection synced =
      pbr::ProjectContactsShell({.detail_open = false, .contacts_unread = 0});
  const pbr::ContactsShellProjection next =
      pbr::ProjectContactsShell({.detail_open = true, .contacts_unread = 5});
  EXPECT_EQ(pbr::ClassifyContactsChromeUpdate(synced, next), pbr::ShellChromeOp::SyncLayout);
}

TEST(ContactsChromeSyncTest, ProjectClampsNegativeUnread) {
  const pbr::ContactsShellProjection projection =
      pbr::ProjectContactsShell({.detail_open = false, .contacts_unread = -4});
  EXPECT_EQ(projection.contacts_unread, 0);
}
