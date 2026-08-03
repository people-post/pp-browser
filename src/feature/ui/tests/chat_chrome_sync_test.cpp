#include "feature/ui/ChatChromeSync.h"

#include <gtest/gtest.h>

TEST(ChatChromeSyncTest, UnchangedIsNone) {
  const pbr::ChatShellProjection layer = pbr::ProjectChatShell({});
  EXPECT_EQ(pbr::ClassifyChatChromeUpdate(layer, layer), pbr::ShellChromeOp::None);
}

TEST(ChatChromeSyncTest, ActiveThreadSyncsLayout) {
  const pbr::ChatShellProjection synced = pbr::ProjectChatShell({.has_active_thread = false});
  const pbr::ChatShellProjection next = pbr::ProjectChatShell({.has_active_thread = true});
  EXPECT_EQ(pbr::ClassifyChatChromeUpdate(synced, next), pbr::ShellChromeOp::SyncLayout);
}

TEST(ChatChromeSyncTest, SessionsUnreadDirtiesNav) {
  const pbr::ChatShellProjection synced =
      pbr::ProjectChatShell({.has_active_thread = true, .sessions_unread = 0});
  const pbr::ChatShellProjection next =
      pbr::ProjectChatShell({.has_active_thread = true, .sessions_unread = 2});
  EXPECT_EQ(pbr::ClassifyChatChromeUpdate(synced, next), pbr::ShellChromeOp::DirtyNav);
}
