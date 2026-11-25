// The settings "?" asks PP 客服 in the half-screen panel (DEV/PLAN-help-to-support-panel.md): whether its question
// goes out (the same text of ours in the last 10 minutes is not sent again, unless that one did not go out), what the
// waiting line says (replying, then after 90 s "got it", nothing once a reply came or while ours has not gone out),
// and when a reply the panel shows is read and not notified.
#include "feature/presenters/SupportAsk.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kMinute = 60'000;
constexpr int64_t kSecond = 1'000;
constexpr int64_t kT0 = 1'700'000'000'000;

SupportMessage Mine(const std::string& text, const int64_t at_ms, const SendMark mark = SendMark::None) {
  return {.own = true, .text = text, .at_ms = at_ms, .mark = mark};
}

SupportMessage Theirs(const std::string& text, const int64_t at_ms) {
  return {.own = false, .text = text, .at_ms = at_ms};
}

// A news read's hidden rows (DEV/PLAN-news-inline-digest.md) are never shown, so the panel never lists them.
TEST(SupportAskTest, HiddenNewsRowsAreNoPanelMessages) {
  ThreadMessage message;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "https://n.example/a\n这篇讲了什么";
  EXPECT_TRUE(SupportMessageOf(message, SendMark::None).has_value());
  for (const char* generation :
       {kNewsRequestGeneration, kNewsDigestGeneration, kNewsDigestTranslatedGeneration, kNewsFailedGeneration}) {
    message.generation = generation;
    EXPECT_FALSE(SupportMessageOf(message, SendMark::None).has_value()) << generation;
  }
}

// --- ShouldAutoSend ----------------------------------------------------------------------------------

TEST(SupportAskTest, TheSameQuestionWithinTenMinutesIsNotSentAgain) {
  const std::vector<SupportMessage> thread = {Mine("「PIN 码」是什么意思？", kT0)};
  EXPECT_FALSE(ShouldAutoSend(thread, "「PIN 码」是什么意思？", kT0 + 1 * kSecond)) << "a second tap";
  EXPECT_FALSE(ShouldAutoSend(thread, "「PIN 码」是什么意思？", kT0 + 9 * kMinute + 59 * kSecond));
  EXPECT_TRUE(ShouldAutoSend(thread, "「PIN 码」是什么意思？", kT0 + 10 * kMinute + 1 * kSecond));
  EXPECT_TRUE(ShouldAutoSend(thread, "「PIN 码」是什么意思？", kT0 + 3 * 60 * kMinute)) << "long after: again";
}

TEST(SupportAskTest, AnotherQuestionIsSent) {
  const std::vector<SupportMessage> thread = {Mine("「PIN 码」是什么意思？", kT0)};
  EXPECT_TRUE(ShouldAutoSend(thread, "「恢复短语」是什么意思？", kT0 + kSecond));
  EXPECT_TRUE(ShouldAutoSend({}, "「恢复短语」是什么意思？", kT0)) << "an empty chat";
}

TEST(SupportAskTest, AFollowUpInBetweenStillCountsTheEarlierSameQuestion) {
  const std::vector<SupportMessage> thread = {Mine("「PIN 码」是什么意思？", kT0),
                                              Theirs("PIN 码是…", kT0 + 20 * kSecond), Mine("谢谢", kT0 + kMinute)};
  EXPECT_FALSE(ShouldAutoSend(thread, "「PIN 码」是什么意思？", kT0 + 2 * kMinute)) << "not only the last message";
}

TEST(SupportAskTest, ASameQuestionThatDidNotGoOutIsSentAgain) {
  const std::vector<SupportMessage> failed = {Mine("「PIN 码」是什么意思？", kT0, SendMark::Failed)};
  EXPECT_TRUE(ShouldAutoSend(failed, "「PIN 码」是什么意思？", kT0 + kMinute));
  // Still being sent: it is on its way, not sent twice.
  const std::vector<SupportMessage> queued = {Mine("「PIN 码」是什么意思？", kT0, SendMark::Queued)};
  EXPECT_FALSE(ShouldAutoSend(queued, "「PIN 码」是什么意思？", kT0 + kSecond));
}

TEST(SupportAskTest, TheirMessageWithTheSameWordsIsNotOurs) {
  const std::vector<SupportMessage> thread = {Theirs("「PIN 码」是什么意思？", kT0)};
  EXPECT_TRUE(ShouldAutoSend(thread, "「PIN 码」是什么意思？", kT0 + kSecond));
}

// --- ReplyState ------------------------------------------------------------------------------------

TEST(SupportAskTest, NoReplyYetIsWaitingThenSlowAtNinetySeconds) {
  const std::vector<SupportMessage> thread = {Mine("q", kT0)};
  EXPECT_EQ(ReplyState(thread, kT0 + kSecond), SupportWait::Waiting);
  EXPECT_EQ(ReplyState(thread, kT0 + 89 * kSecond), SupportWait::Waiting);
  EXPECT_EQ(ReplyState(thread, kT0 + 90 * kSecond), SupportWait::Slow);
  EXPECT_EQ(ReplyState(thread, kT0 + 60 * kMinute), SupportWait::Slow) << "still waiting, never ends by itself";
}

TEST(SupportAskTest, ANewerMessageOfTheirsIsTheAnswer) {
  EXPECT_EQ(ReplyState({Mine("q", kT0), Theirs("a", kT0 + 15 * kSecond)}, kT0 + 16 * kSecond), SupportWait::Answered);
  EXPECT_EQ(ReplyState({}, kT0), SupportWait::Answered) << "nothing asked: nothing to wait for";
}

TEST(SupportAskTest, AnOlderMessageOfTheirsIsNoAnswer) {
  const std::vector<SupportMessage> thread = {Theirs("old answer", kT0 - kMinute), Mine("q", kT0)};
  EXPECT_EQ(ReplyState(thread, kT0 + kSecond), SupportWait::Waiting);
}

TEST(SupportAskTest, TwoQuestionsWithNoReplyAreWaitingFromTheFirst) {
  const std::vector<SupportMessage> thread = {Mine("q1", kT0), Mine("q2", kT0 + 60 * kSecond)};
  EXPECT_EQ(ReplyState(thread, kT0 + 61 * kSecond), SupportWait::Waiting);
  EXPECT_EQ(ReplyState(thread, kT0 + 90 * kSecond), SupportWait::Slow) << "timed from the first unanswered question";
}

// The accepted inaccuracy (risk 2): one reply after both questions clears the wait, though it may answer only the first.
TEST(SupportAskTest, OneReplyAfterTwoQuestionsIsAnswered) {
  const std::vector<SupportMessage> thread = {Mine("q1", kT0), Mine("q2", kT0 + kSecond),
                                              Theirs("a1", kT0 + 20 * kSecond)};
  EXPECT_EQ(ReplyState(thread, kT0 + 21 * kSecond), SupportWait::Answered);
}

TEST(SupportAskTest, AReplyThenAnotherQuestionWaitsForThatQuestion) {
  const std::vector<SupportMessage> thread = {Mine("q1", kT0), Theirs("a1", kT0 + 20 * kSecond),
                                              Mine("q2", kT0 + 30 * kSecond)};
  EXPECT_EQ(ReplyState(thread, kT0 + 31 * kSecond), SupportWait::Waiting);
  EXPECT_EQ(ReplyState(thread, kT0 + 119 * kSecond), SupportWait::Waiting) << "timed from q2, not q1";
  EXPECT_EQ(ReplyState(thread, kT0 + 120 * kSecond), SupportWait::Slow);
}

// Offline (risk 4): ours has not gone out, so nobody is replying yet: its own clock / red mark tells, no waiting line.
TEST(SupportAskTest, AQuestionThatHasNotGoneOutIsNotWaitedFor) {
  EXPECT_EQ(ReplyState({Mine("q", kT0, SendMark::Queued)}, kT0 + kSecond), SupportWait::NotSent);
  EXPECT_EQ(ReplyState({Mine("q", kT0, SendMark::Failed)}, kT0 + 2 * kMinute), SupportWait::NotSent)
      << "never the 90 s line for a message that did not go out";
  EXPECT_EQ(ReplyState({Mine("q1", kT0), Mine("q2", kT0 + kSecond, SendMark::Failed)}, kT0 + 2 * kMinute),
            SupportWait::NotSent);
}

// --- read / notify -----------------------------------------------------------------------------------

TEST(SupportAskTest, AReplyIsReadOnlyWhileThePanelShowsItToSomeoneLooking) {
  EXPECT_TRUE(ShouldReadSupportThread(/*attentive=*/true, /*panel_shows_thread=*/true));
  EXPECT_FALSE(ShouldReadSupportThread(true, false)) << "the panel is closed or shows another chat";
  EXPECT_FALSE(ShouldReadSupportThread(false, true)) << "the phone was put away with the panel open";
  EXPECT_FALSE(ShouldReadSupportThread(false, false));
}

// The app raises a notice only when nobody looks (BackgroundNoticeFor): a reply read in the panel is not notified, one
// that nobody saw is.
TEST(SupportAskTest, AReplyReadInThePanelIsNotNotifiedOneNobodySawIs) {
  EXPECT_TRUE(ShouldSuppressNotice(/*attentive=*/true, /*panel_shows_thread=*/true));
  EXPECT_FALSE(ShouldSuppressNotice(true, false));
  EXPECT_FALSE(ShouldSuppressNotice(false, true)) << "put away with the panel open: the notice is the only way to know";
  EXPECT_FALSE(ShouldSuppressNotice(false, false));
}

// --- what counts as a message in the conversation -------------------------------------------------------

TEST(SupportAskTest, OnlyMessageBubblesCountOursMarkedAsTheyAre) {
  ThreadMessage ours;
  ours.sender_contact_id = kLocalSelfContactId;
  ours.text = "q";
  ours.timestamp = kT0;
  const std::optional<SupportMessage> mine = SupportMessageOf(ours, SendMark::Failed);
  ASSERT_TRUE(mine.has_value());
  EXPECT_TRUE(mine->own);
  EXPECT_EQ(mine->text, "q");
  EXPECT_EQ(mine->at_ms, kT0);
  EXPECT_EQ(mine->mark, SendMark::Failed);

  ThreadMessage theirs = ours;
  theirs.sender_contact_id = "c-support";
  const std::optional<SupportMessage> reply = SupportMessageOf(theirs, SendMark::None);
  ASSERT_TRUE(reply.has_value());
  EXPECT_FALSE(reply->own);

  ThreadMessage line = theirs;
  line.content_type = ChatContentType::System; // "did not go out", a date line: no reply
  EXPECT_FALSE(SupportMessageOf(line, SendMark::None).has_value());
  ThreadMessage gone = theirs;
  gone.content_type = ChatContentType::Deleted;
  EXPECT_FALSE(SupportMessageOf(gone, SendMark::None).has_value());
}

} // namespace
} // namespace pbr
