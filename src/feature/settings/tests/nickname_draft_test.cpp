#include "feature/settings/NicknameDraft.h"

#include <gtest/gtest.h>

TEST(NicknameDraftTest, HydrateThenEditThenCommit) {
  pbr::NicknameDraft nick;
  EXPECT_FALSE(nick.ShouldCommit());

  nick.OnHydrated("alice");
  EXPECT_TRUE(nick.ready);
  EXPECT_FALSE(nick.edited);
  EXPECT_EQ(nick.draft, "alice");
  EXPECT_FALSE(nick.ShouldCommit());

  nick.OnUserEdit("alice2");
  EXPECT_TRUE(nick.ShouldCommit());

  nick.OnCommitSuccess("alice2");
  EXPECT_FALSE(nick.edited);
  EXPECT_EQ(nick.committed, "alice2");
  EXPECT_FALSE(nick.ShouldCommit());
}

TEST(NicknameDraftTest, RemountDoesNotWipeInProgressEdit) {
  pbr::NicknameDraft nick;
  nick.OnHydrated("alice");
  nick.OnUserEdit("bob");

  nick.OnHydrated("alice"); // session still has old value
  EXPECT_EQ(nick.draft, "bob");
  EXPECT_TRUE(nick.edited);
  EXPECT_TRUE(nick.ShouldCommit());
}

TEST(NicknameDraftTest, IdentityUnavailableDoesNotClearReadyNickname) {
  pbr::NicknameDraft nick;
  nick.OnHydrated("alice");
  nick.OnIdentityUnavailable();
  EXPECT_TRUE(nick.ready);
  EXPECT_EQ(nick.committed, "alice");
  EXPECT_EQ(nick.draft, "alice");
}

TEST(NicknameDraftTest, EmptyBeforeReadyNeverCommits) {
  pbr::NicknameDraft nick;
  nick.OnUserEdit(""); // ignored until ready
  EXPECT_FALSE(nick.edited);
  EXPECT_FALSE(nick.ShouldCommit());

  nick.OnIdentityUnavailable();
  EXPECT_FALSE(nick.ShouldCommit());

  nick.OnHydrated("alice");
  EXPECT_EQ(nick.draft, "alice");
  EXPECT_FALSE(nick.ShouldCommit());
}

TEST(NicknameDraftTest, IntentionalClearAfterReadyCommits) {
  pbr::NicknameDraft nick;
  nick.OnHydrated("alice");
  nick.OnUserEdit("");
  EXPECT_TRUE(nick.ShouldCommit());
  nick.OnCommitSuccess("");
  EXPECT_EQ(nick.committed, "");
  EXPECT_FALSE(nick.edited);
}
