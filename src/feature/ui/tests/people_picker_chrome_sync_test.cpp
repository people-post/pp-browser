#include "feature/ui/contacts/PeoplePickerChromeSync.h"

#include <gtest/gtest.h>

TEST(PeoplePickerChromeSyncTest, UnchangedIsNone) {
  const pbr::PeoplePickerShellProjection layer = pbr::ProjectPeoplePickerShell({});
  EXPECT_EQ(pbr::ClassifyPeoplePickerChromeUpdate(layer, layer), pbr::ShellChromeOp::None);
}

TEST(PeoplePickerChromeSyncTest, OverlayOpenSyncsLayout) {
  const pbr::PeoplePickerShellProjection synced = pbr::ProjectPeoplePickerShell({.overlay_open = false});
  const pbr::PeoplePickerShellProjection next = pbr::ProjectPeoplePickerShell({.overlay_open = true});
  EXPECT_EQ(pbr::ClassifyPeoplePickerChromeUpdate(synced, next), pbr::ShellChromeOp::SyncLayout);
}
