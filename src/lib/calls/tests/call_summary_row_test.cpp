// The transcript's call row: who started it (side), voice or video (icon), duration or outcome.
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallControlFamily.h"
#include "lib/calls/CallTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ThreadMessage Summary(const std::string& sender, CallMediaMode mode, CallOutcome outcome,
                      std::optional<int64_t> duration_ms, bool group = false, bool started = false) {
  CallSummaryDetail detail;
  detail.call_id = "call:row";
  detail.media_mode = mode;
  detail.outcome = outcome;
  detail.duration_ms = duration_ms;
  detail.group = group;
  detail.started = started;
  auto encoded = CallControlCodec::EncodeSummary(detail);
  EXPECT_TRUE(encoded);
  auto message = CallControlCodec::BuildSystemMessage("thread-row", CallControlType::CallSummary, "call",
                                                      encoded ? *encoded : std::string("{}"), sender);
  EXPECT_TRUE(message);
  return message ? *message : ThreadMessage{};
}

TEST(CallSummaryRowTest, OurCompletedVoiceCallIsOursWithItsDuration) {
  const auto row = CallSummaryRowOf(Summary(kLocalSelfContactId, CallMediaMode::Voice, CallOutcome::Completed, 71'000));
  ASSERT_TRUE(row);
  EXPECT_TRUE(row->ours);
  EXPECT_FALSE(row->video);
  EXPECT_TRUE(row->connected);
  EXPECT_EQ(row->text, "1:11");
}

TEST(CallSummaryRowTest, TheirVideoCallSitsOnTheirSide) {
  const auto row = CallSummaryRowOf(Summary("account:peer", CallMediaMode::Video, CallOutcome::Completed, 56'000));
  ASSERT_TRUE(row);
  EXPECT_FALSE(row->ours);
  EXPECT_TRUE(row->video);
  EXPECT_TRUE(row->connected);
  EXPECT_EQ(row->text, "0:56");
}

TEST(CallSummaryRowTest, OutcomesWithoutADurationAreNotConnected) {
  for (CallOutcome outcome : {CallOutcome::Missed, CallOutcome::Cancelled, CallOutcome::Declined, CallOutcome::Failed}) {
    const auto row = CallSummaryRowOf(Summary("account:peer", CallMediaMode::Voice, outcome, std::nullopt));
    ASSERT_TRUE(row);
    EXPECT_FALSE(row->connected);
    EXPECT_FALSE(row->text.empty());
    EXPECT_EQ(row->text.find(':'), std::string::npos) << row->text;
  }
}

TEST(CallSummaryRowTest, NotASummary) {
  ThreadMessage plain;
  plain.content_type = ChatContentType::Text;
  plain.text = "hello";
  EXPECT_FALSE(CallSummaryRowOf(plain));
}

} // namespace
} // namespace pbr
