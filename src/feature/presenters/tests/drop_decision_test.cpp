// DEV/PLAN-desktop-drop-paste.md, C: where a drop lands (inside the chat / outside / no place known) against what the
// chat takes now, and what the hover hint says. Pure.
#include "feature/presenters/AttachIntake.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

constexpr AttachRefusal kAllRefusals[] = {
    AttachRefusal::None,       AttachRefusal::NotReady,      AttachRefusal::NoChat,   AttachRefusal::AiComposer,
    AttachRefusal::Editing,    AttachRefusal::InputDisabled, AttachRefusal::SendHeld, AttachRefusal::PreviewOpen,
    AttachRefusal::LayerAbove, AttachRefusal::PasteInFlight, AttachRefusal::Busy,     AttachRefusal::AttachUnavailable,
};

TEST(DropDecisionTest, InsideTheChatAndSendableIsTaken) {
  EXPECT_EQ(DecideDrop(true, true, AttachRefusal::None), DropVerdict::Take);
}

TEST(DropDecisionTest, InsideTheChatButRefusedGoesToTheRefusal) {
  for (const AttachRefusal refusal : kAllRefusals) {
    if (refusal == AttachRefusal::None) {
      continue;
    }
    EXPECT_EQ(DecideDrop(true, true, refusal), DropVerdict::Refuse) << static_cast<int>(refusal);
  }
}

TEST(DropDecisionTest, OutsideTheChatTakesNothingWhateverTheChatWouldSay) {
  for (const AttachRefusal refusal : kAllRefusals) {
    EXPECT_EQ(DecideDrop(true, false, refusal), DropVerdict::Outside) << static_cast<int>(refusal);
  }
}

TEST(DropDecisionTest, NoPositionIsNeverTaken) {
  for (const AttachRefusal refusal : kAllRefusals) {
    EXPECT_EQ(DecideDrop(false, true, refusal), DropVerdict::NoPosition) << static_cast<int>(refusal);
    EXPECT_EQ(DecideDrop(false, false, refusal), DropVerdict::NoPosition) << static_cast<int>(refusal);
  }
}

TEST(DropDecisionTest, OnlyTakeOpensThePreview) {
  for (const bool has_position : {false, true}) {
    for (const bool inside : {false, true}) {
      for (const AttachRefusal refusal : kAllRefusals) {
        const DropVerdict verdict = DecideDrop(has_position, inside, refusal);
        const bool should_take = has_position && inside && refusal == AttachRefusal::None;
        EXPECT_EQ(verdict == DropVerdict::Take, should_take);
      }
    }
  }
}

TEST(DropHintTest, InsideAndSendableSaysDropToSend) {
  EXPECT_STREQ(DropHintKey(true, AttachRefusal::None), "chat.drop.hint");
}

TEST(DropHintTest, InsideButRefusedShowsTheRefusalsOwnLine) {
  for (const AttachRefusal refusal : kAllRefusals) {
    if (refusal == AttachRefusal::None) {
      continue;
    }
    const char* key = DropHintKey(true, refusal);
    if (AttachRefusalKey(refusal) == nullptr) {
      EXPECT_EQ(key, nullptr) << "locked / a paste being written: no hint, as the pill says nothing either";
    } else {
      EXPECT_STREQ(key, AttachRefusalKey(refusal));
    }
  }
  EXPECT_STREQ(DropHintKey(true, AttachRefusal::PreviewOpen), "chat.attach.finish_current");
}

TEST(DropDecisionTest, AChatWithoutAPlusButtonRefusesADropWithTheNotHereLine) {
  AttachState state;
  state.attach_unavailable = true; // a dissolved group (G016), or messaging / the mesh not ready
  const AttachRefusal refusal = RefusalFor(state);
  EXPECT_EQ(refusal, AttachRefusal::AttachUnavailable);
  EXPECT_EQ(DecideDrop(true, true, refusal), DropVerdict::Refuse);
  EXPECT_STREQ(DropHintKey(true, refusal), "chat.attach.not_here");
  EXPECT_STREQ(AttachRefusalKey(refusal), "chat.attach.not_here");
  EXPECT_TRUE(DecidePaste(ClipboardSummary{.has_files = true, .files_enabled = true}, refusal).take_over);
  EXPECT_STREQ(DecidePaste(ClipboardSummary{.has_files = true, .files_enabled = true}, refusal).pill_key,
               "chat.attach.not_here");
}

TEST(DropHintTest, OutsideTheChatThereIsNoHint) {
  for (const AttachRefusal refusal : kAllRefusals) {
    EXPECT_EQ(DropHintKey(false, refusal), nullptr);
  }
}

} // namespace
} // namespace pbr
