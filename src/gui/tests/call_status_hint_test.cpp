#include "gui/CallStatusHint.h"

#include <gtest/gtest.h>

#include <vector>

using pbr::P2pStatusHintKeys;

TEST(CallStatusHintTest, NamesACauseOnlyWithEvidence) {
  EXPECT_EQ(P2pStatusHintKeys(false, false), (std::vector<const char*>{"call.hint.peer_unreachable"}));
  EXPECT_EQ(P2pStatusHintKeys(false, true), (std::vector<const char*>{"call.hint.seed_unreachable"}));
  EXPECT_EQ(P2pStatusHintKeys(true, false), (std::vector<const char*>{"hints.mic_blocked"}));
  EXPECT_EQ(P2pStatusHintKeys(true, true),
            (std::vector<const char*>{"call.hint.seed_unreachable", "hints.mic_blocked"}));
}
