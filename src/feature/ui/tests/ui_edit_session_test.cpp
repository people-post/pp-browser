#include "feature/ui/UiEditSession.h"

#include <gtest/gtest.h>

TEST(UiEditSessionTest, CommitOnlyWhenLiveDiffersAndNotRemounting) {
  pbr::UiEditSession session;
  const std::string field = "nickname";

  session.OnLoaded(field, "alice");
  EXPECT_FALSE(session.ShouldCommit(field, "alice"));
  EXPECT_TRUE(session.ShouldCommit(field, "bob"));

  session.BeginRemount();
  EXPECT_FALSE(session.ShouldCommit(field, "bob"));
  session.EndRemount();
  EXPECT_TRUE(session.ShouldCommit(field, "bob"));

  session.OnCommitted(field, "bob");
  EXPECT_FALSE(session.ShouldCommit(field, "bob"));
}

TEST(UiEditSessionTest, MidEditBlocksPushPreservesLiveOnLoad) {
  pbr::UiEditSession session;
  const std::string field = "nickname";

  session.OnLoaded(field, "alice");
  EXPECT_TRUE(session.ShouldPushToView(field, "alice"));
  EXPECT_FALSE(session.ShouldPushToView(field, "alic"));

  EXPECT_EQ(session.ResolveAfterLoad(field, "alice", "alic"), "alic");
  session.OnLoaded(field, "alice");
  EXPECT_EQ(session.ResolveAfterLoad(field, "alice", "alic"), "alic");
}

TEST(UiEditSessionTest, RemountEmptyLiveResolvesToLoaded) {
  pbr::UiEditSession session;
  const std::string field = "nickname";

  session.OnLoaded(field, "alice");
  // Remount clears the binding before Dirty/SetValue; must not keep "" as mid-edit.
  session.BeginRemount();
  EXPECT_FALSE(session.IsMidEdit(field, ""));
  EXPECT_EQ(session.ResolveAfterLoad(field, "alice", ""), "alice");
  EXPECT_TRUE(session.ShouldPushToView(field, ""));
  session.EndRemount();

  // After remount settles, an intentional clear is mid-edit and may commit.
  EXPECT_TRUE(session.IsMidEdit(field, ""));
  EXPECT_TRUE(session.ShouldCommit(field, ""));
}

TEST(UiEditSessionTest, EmptyLiveWithoutBaselineResolvesToLoaded) {
  pbr::UiEditSession session;
  const std::string field = "nickname";

  // First hydrate: empty binding must not look mid-edit vs a just-loaded value.
  EXPECT_EQ(session.ResolveAfterLoad(field, "alice", ""), "alice");
  EXPECT_FALSE(session.IsMidEdit(field, ""));
  session.OnLoaded(field, "alice");
  EXPECT_TRUE(session.ShouldPushToView(field, "alice"));
}

TEST(UiEditSessionTest, NestedRemountDepth) {
  pbr::UiEditSession session;
  EXPECT_FALSE(session.RemountBlocking());
  session.BeginRemount();
  session.BeginRemount();
  EXPECT_TRUE(session.RemountBlocking());
  session.EndRemount();
  EXPECT_TRUE(session.RemountBlocking());
  session.EndRemount();
  EXPECT_FALSE(session.RemountBlocking());
}

TEST(UiEditSessionTest, NoBaselineNeverCommits) {
  pbr::UiEditSession session;
  EXPECT_FALSE(session.ShouldCommit("missing", ""));
  EXPECT_FALSE(session.ShouldCommit("missing", "x"));
}
