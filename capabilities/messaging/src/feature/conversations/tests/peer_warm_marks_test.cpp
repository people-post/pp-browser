// The warm mark on a peer's link (call invite / chat): put on at once, taken off once the peer has been quiet for
// the spell, kept while calls or messages continue (#577).
#include "feature/conversations/PeerWarmMarks.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

struct Marks {
  std::vector<std::string> marked;
  std::vector<std::string> cleared;
  PeerWarmMarks marks{1000};

  Marks() {
    marks.Bind([this](const std::string& key) { marked.push_back(key); },
               [this](const std::string& key) { cleared.push_back(key); });
  }
};

using Keys = std::vector<std::string>;

TEST(PeerWarmMarksTest, ACallInviteMarksThePeerAtOnce) {
  Marks m;
  m.marks.Warm("QmBob", 0); // ConversationsHub::PrefetchPeerReachability → WarmPeerByKey → PeerTransports::WarmByKey
  EXPECT_EQ(m.marked, Keys{"QmBob"});
  EXPECT_TRUE(m.marks.IsWarm("QmBob"));
  m.marks.Warm("QmBob", 10);
  EXPECT_EQ(m.marked, (Keys{"QmBob", "QmBob"})) << "marked again: the link may be a new one";
  EXPECT_EQ(m.cleared, Keys{});
}

TEST(PeerWarmMarksTest, TheMarkComesOffAfterTheSpellWithNoActivity) {
  Marks m;
  m.marks.Warm("QmBob", 0);
  m.marks.Tick(999);
  EXPECT_EQ(m.cleared, Keys{});
  m.marks.Tick(1000);
  EXPECT_EQ(m.cleared, Keys{"QmBob"});
  EXPECT_FALSE(m.marks.IsWarm("QmBob"));
  m.marks.Tick(5000);
  EXPECT_EQ(m.cleared, Keys{"QmBob"}) << "cleared once";
}

TEST(PeerWarmMarksTest, MessagesAndCallsKeepTheMarkOn) {
  Marks m;
  m.marks.Warm("QmBob", 0);
  m.marks.Touch("QmBob", 800); // a message over the link
  m.marks.Tick(1500);
  EXPECT_EQ(m.cleared, Keys{}) << "1000 ms from the message";
  m.marks.Warm("QmBob", 1700); // another call
  m.marks.Tick(2600);
  EXPECT_EQ(m.cleared, Keys{});
  m.marks.Tick(2700);
  EXPECT_EQ(m.cleared, Keys{"QmBob"});
  m.marks.Touch("QmBob", 2800);
  EXPECT_FALSE(m.marks.IsWarm("QmBob")) << "a message after the spell does not put it back";
  EXPECT_EQ(m.marked, (Keys{"QmBob", "QmBob"}));
}

TEST(PeerWarmMarksTest, AResetForgetsEveryPeerWithTheMeshThatMarkedThem) {
  Marks m;
  m.marks.Warm("QmBob", 0);
  m.marks.Reset();
  m.marks.Tick(5000);
  EXPECT_EQ(m.cleared, Keys{}) << "its links went with the mesh";
  EXPECT_FALSE(m.marks.IsWarm("QmBob"));
}

} // namespace
} // namespace pbr
