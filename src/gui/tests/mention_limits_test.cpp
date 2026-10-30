#include "lib/messaging/GroupMentions.h"
#include "lib/ui/MentionDraft.h"

#include <gtest/gtest.h>

namespace pbr {

// The composer (lib/ui) and the codec (lib/messaging) cannot include each other; this is where both are visible.
static_assert(kMaxDraftMentions == kMaxMentionsPerMessage, "composer and codec mention limits drifted apart");

TEST(MentionLimitsTest, ComposerAndCodecAgree) {
  EXPECT_EQ(kMaxDraftMentions, kMaxMentionsPerMessage);
}

} // namespace pbr
