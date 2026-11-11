// The wait between choosing an attachment and seeing it (DEV/PLAN-attachment-bubbles-multi-pick.md step 3): when the
// centred "Preparing…" overlay shows, each of the plan's seven ways a pick ends, and the pick serial that drops late
// results.
#include "feature/presenters/AttachmentPicks.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kT0 = 1'000'000;

/** A pick armed in chat "a" and being prepared since kT0. */
AttachmentPicks Preparing(uint64_t& serial) {
  AttachmentPicks picks;
  serial = picks.Arm("a");
  picks.Show(serial, kT0);
  return picks;
}

PickOverlay Overlay(const AttachmentPicks& picks, const std::string& open = "a", const bool choice = false,
                    const bool bubble = false, const int64_t now = kT0 + 100) {
  return PickOverlayOf(picks.State(), open, choice, bubble, now);
}

TEST(AttachmentPicksTest, APickBeingPreparedShowsOneLineInItsChatOnly) {
  uint64_t serial = 0;
  const AttachmentPicks picks = Preparing(serial);
  EXPECT_EQ(Overlay(picks), PickOverlay::OneLine);
  EXPECT_EQ(Overlay(picks, "b"), PickOverlay::Hidden) << "another chat open";
  EXPECT_EQ(Overlay(picks, "a"), PickOverlay::OneLine) << "back in its chat: still there";
  AttachmentPicks armed;
  armed.Arm("a");
  EXPECT_EQ(Overlay(armed), PickOverlay::Hidden) << "the picker is open, nothing is being prepared yet";
}

TEST(AttachmentPicksTest, AfterFiveSecondsASecondLine) {
  uint64_t serial = 0;
  const AttachmentPicks picks = Preparing(serial);
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs - 1), PickOverlay::OneLine);
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs), PickOverlay::TwoLines);
}

// The seven ends: after each, nothing shows — also when "more than five seconds" is true by then.
TEST(AttachmentPicksTest, End1ThePictureChoiceShows) {
  uint64_t serial = 0;
  AttachmentPicks picks = Preparing(serial);
  EXPECT_EQ(Overlay(picks, "a", /*choice=*/true), PickOverlay::Hidden);
  picks.End(serial);
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs + 1), PickOverlay::Hidden);
}

TEST(AttachmentPicksTest, End2TheFileOrVideoBubbleIsBuilt) {
  uint64_t serial = 0;
  AttachmentPicks picks = Preparing(serial);
  EXPECT_EQ(Overlay(picks, "a", false, /*bubble=*/true), PickOverlay::Hidden);
  picks.End(serial);
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs + 1), PickOverlay::Hidden);
}

TEST(AttachmentPicksTest, End3ThePickerWasCancelled) {
  uint64_t serial = 0;
  AttachmentPicks picks = Preparing(serial);
  picks.End(serial); // the callback brought no path
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs + 1), PickOverlay::Hidden);
}

TEST(AttachmentPicksTest, End4ReadingCopyingOrPreparingFailed) {
  uint64_t serial = 0;
  AttachmentPicks picks = Preparing(serial);
  picks.End(serial);
  EXPECT_FALSE(picks.IsCurrent(serial));
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs + 1), PickOverlay::Hidden);
}

TEST(AttachmentPicksTest, End5TheOverlaysCancel) {
  uint64_t serial = 0;
  AttachmentPicks picks = Preparing(serial);
  picks.EndCurrent();
  EXPECT_FALSE(picks.IsCurrent(serial)) << "its serial is void: what comes back is dropped";
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs + 1), PickOverlay::Hidden);
}

TEST(AttachmentPicksTest, End6RefusedBecauseAnotherAttachmentIsSending) {
  uint64_t serial = 0;
  AttachmentPicks picks = Preparing(serial);
  EXPECT_FALSE(picks.Accept(serial, /*busy=*/true));
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs + 1), PickOverlay::Hidden);
  uint64_t other = 0;
  AttachmentPicks free = Preparing(other);
  EXPECT_TRUE(free.Accept(other, /*busy=*/false));
  EXPECT_TRUE(free.IsCurrent(other)) << "accepted: it goes on until its choice or bubble";
}

TEST(AttachmentPicksTest, End7LockSignOutOrTheChatDeleted) {
  uint64_t serial = 0;
  AttachmentPicks picks = Preparing(serial);
  picks.EndCurrent();
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + kPickSlowMs + 1), PickOverlay::Hidden);
}

// 8a: cancel, pick again at once, the old result arrives first: it is dropped and its files released (each once, by
// path), the new pick is not affected.
TEST(AttachmentPicksTest, ALateResultOfACancelledPickIsDroppedAndItsFilesReleasedOnce) {
  AttachmentPicks picks;
  const uint64_t first = picks.Arm("a");
  picks.Show(first, kT0);
  picks.EndCurrent(); // Cancel on the overlay
  const uint64_t second = picks.Arm("a");
  picks.Show(second, kT0 + 50);
  std::vector<std::string> released;
  const auto release = [&released](const std::string& path) { released.push_back(path); };
  EXPECT_FALSE(picks.TakeResult(first, "a", {"/tmp/pp-picks/old.jpg"}, release).has_value())
      << "the copy of the old pick";
  EXPECT_FALSE(picks.TakeResult(first, "a", {"/picked/upload-thumb-3.jpg"}, release).has_value())
      << "its video's poster";
  EXPECT_EQ(released, (std::vector<std::string>{"/tmp/pp-picks/old.jpg", "/picked/upload-thumb-3.jpg"}));
  EXPECT_TRUE(picks.IsCurrent(second));
  EXPECT_EQ(Overlay(picks, "a", false, false, kT0 + 60), PickOverlay::OneLine);
  EXPECT_EQ(picks.TakeResult(second, "a", {"/tmp/pp-picks/new.jpg"}, release), std::optional<std::string>("a"));
  EXPECT_EQ(released.size(), 2u) << "the current pick's file is kept";
}

// 8b: a pick while another attachment is still preparing / sending: refused, no overlay left behind (the controller
// then says "The previous attachment is still being sent" and builds no second item).
TEST(AttachmentPicksTest, APickWhileAnotherIsSendingLeavesNoOverlay) {
  AttachmentPicks picks;
  const uint64_t serial = picks.Arm("a");
  picks.Show(serial, kT0);
  ASSERT_FALSE(picks.Accept(serial, /*busy=*/true));
  EXPECT_FALSE(picks.IsCurrent(serial));
  EXPECT_EQ(Overlay(picks), PickOverlay::Hidden);
  std::vector<std::string> released;
  EXPECT_FALSE(picks
                   .TakeResult(serial, "a", {"/tmp/pp-picks/x.mov"},
                               [&released](const std::string& path) { released.push_back(path); })
                   .has_value());
  EXPECT_EQ(released, (std::vector<std::string>{"/tmp/pp-picks/x.mov"}));
}

TEST(AttachmentPicksTest, ANewPickReplacesAnOlderOne) {
  AttachmentPicks picks;
  const uint64_t first = picks.Arm("a");
  const uint64_t second = picks.Arm("b");
  EXPECT_GT(second, first);
  EXPECT_FALSE(picks.IsCurrent(first));
  picks.Show(first, kT0); // the old pick's preparing notice: ignored
  EXPECT_FALSE(picks.State().showing);
  picks.End(first); // ending the old one leaves the new one
  EXPECT_TRUE(picks.IsCurrent(second));
}

// B1: a pick's result goes only to the chat it was armed in, and only while that chat is open.
TEST(AttachmentPicksTest, AResultArrivingWhileAnotherChatIsOpenIsDroppedAndReleased) {
  AttachmentPicks picks;
  const uint64_t serial = picks.Arm("a");
  picks.Show(serial, kT0);
  std::vector<std::string> released;
  const auto release = [&released](const std::string& path) { released.push_back(path); };
  EXPECT_FALSE(picks.TakeResult(serial, "b", {"/tmp/pp-picks/report.pdf"}, release).has_value())
      << "never sent to the chat open now, nor in the background to its own";
  EXPECT_EQ(released, (std::vector<std::string>{"/tmp/pp-picks/report.pdf"}));
  EXPECT_FALSE(picks.IsCurrent(serial)) << "the pick is over";
  EXPECT_EQ(Overlay(picks, "a"), PickOverlay::Hidden);
}

TEST(AttachmentPicksTest, AResultArrivingInTheOriginatingChatGoesToThatThread) {
  AttachmentPicks picks;
  const uint64_t serial = picks.Arm("a");
  std::vector<std::string> released;
  const auto taken = picks.TakeResult(serial, "a", {"/tmp/pp-picks/report.pdf"},
                                      [&released](const std::string& path) { released.push_back(path); });
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(*taken, "a");
  EXPECT_TRUE(released.empty());
  AttachmentPicks nowhere;
  const uint64_t no_chat = nowhere.Arm("");
  EXPECT_FALSE(nowhere.TakeResult(no_chat, "", {}, nullptr).has_value()) << "armed with no chat: nowhere to send";
}

// Re-review item 1: a one-second blip (a handover) never stops an upload; offline held for 3 s does; online in between
// starts the count again.
TEST(OfflineHoldTest, OnlyOfflineHeldForThreeSecondsStops) {
  OfflineHold hold;
  EXPECT_FALSE(hold.Update(false, 0));
  EXPECT_FALSE(hold.Update(true, 1000));
  EXPECT_FALSE(hold.Update(true, 2000)) << "a 1 s blip";
  EXPECT_FALSE(hold.Update(false, 2100)) << "back online: the count starts again";
  EXPECT_FALSE(hold.Update(true, 3000));
  EXPECT_FALSE(hold.Update(true, 5999));
  EXPECT_TRUE(hold.Update(true, 6000)) << "3 s without a break";
  EXPECT_TRUE(hold.Update(true, 9000));
  EXPECT_FALSE(hold.Update(false, 9100));
}

// Re-review item 4: a result dropped because its chat was left is said (when it brought a file); a stale one is not.
TEST(AttachmentPicksTest, LeavingTheChatIsReportedOnlyForTheCurrentPick) {
  AttachmentPicks picks;
  const uint64_t serial = picks.Arm("a");
  EXPECT_FALSE(picks.LeftChat(serial, "a"));
  EXPECT_TRUE(picks.LeftChat(serial, "b"));
  picks.EndCurrent();
  EXPECT_FALSE(picks.LeftChat(serial, "b")) << "an ended pick's result is dropped silently (it was cancelled)";
}

} // namespace
} // namespace pbr
