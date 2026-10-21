// Call control as a system-control family: signaling on the lane, one summary line per call.
#include "feature/calls/CallControlFamily.h"

#include "domain/calls/CallControlCodec.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class CallControlFamilyTest : public ::testing::Test {
protected:
  void SetUp() override {
#ifdef PP_BROWSER_ASSETS_DIR
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
#endif
    LocalizationService::Instance().SetPreferredLanguage("en");
    labels_.peer = [](const std::string& identity) { return identity == "account:bob" ? "Bob" : ""; };
  }

  static ThreadMessage Message(CallControlType type, const std::string& sender, const std::string& detail = "{}") {
    return CallControlCodec::BuildSystemMessage("thread:t", type, "text", detail, sender).value();
  }

  static ThreadMessage Summary(const std::string& sender, CallOutcome outcome, std::optional<int64_t> duration_ms,
                               bool group = false, CallMediaMode media = CallMediaMode::Voice) {
    CallSummaryDetail summary;
    summary.call_id = "call:s";
    summary.media_mode = media;
    summary.outcome = outcome;
    summary.duration_ms = duration_ms;
    summary.group = group;
    return Message(CallControlType::CallSummary, sender, CallControlCodec::EncodeSummary(summary).value());
  }

  std::optional<std::string> Line(const ThreadMessage& message) {
    return MakeCallControlFamily({}).line(message, labels_);
  }

  SystemControlLabels labels_;
};

TEST_F(CallControlFamilyTest, SignalingRidesTheLaneTheSummaryTheStream) {
  const SystemControlFamily family = MakeCallControlFamily({});
  EXPECT_TRUE(family.on_lane(Message(CallControlType::CallInvite, "account:bob")));
  EXPECT_TRUE(family.on_lane(Message(CallControlType::CallMediaKey, "account:bob")));
  EXPECT_TRUE(family.on_lane(Message(CallControlType::CallRoster, "account:bob")));
  EXPECT_FALSE(family.on_lane(Summary("account:bob", CallOutcome::Completed, 1000)));
  EXPECT_GE(family.lane_max_age_ms, kDefaultCallInviteTtlMs) << "an invite rings for a minute";
  EXPECT_FALSE(family.line(Message(CallControlType::CallInvite, "account:bob"), labels_)) << "signaling has no line";
}

TEST_F(CallControlFamilyTest, OneLinePerCallFromTheReadersSide) {
  EXPECT_EQ(Line(Summary(kLocalSelfContactId, CallOutcome::Completed, 201'000)),
            std::optional<std::string>("Outgoing voice call · 3:21"));
  EXPECT_EQ(Line(Summary("account:bob", CallOutcome::Completed, 201'000)),
            std::optional<std::string>("Incoming voice call · 3:21"));
  EXPECT_EQ(Line(Summary("account:bob", CallOutcome::Missed, std::nullopt)),
            std::optional<std::string>("Missed voice call"));
  EXPECT_EQ(Line(Summary("account:bob", CallOutcome::Cancelled, std::nullopt)),
            std::optional<std::string>("Missed voice call")) << "hung up before we answered: we missed it";
  EXPECT_EQ(Line(Summary(kLocalSelfContactId, CallOutcome::Missed, std::nullopt)),
            std::optional<std::string>("Voice call · no answer"));
  EXPECT_EQ(Line(Summary(kLocalSelfContactId, CallOutcome::Declined, std::nullopt, false, CallMediaMode::Video)),
            std::optional<std::string>("Video call · declined"));
  EXPECT_EQ(Line(Summary("account:bob", CallOutcome::Completed, 61'000, true)),
            std::optional<std::string>("Bob started a voice call · 1:01"));
  const auto unknown = Line(Summary("account:carol", CallOutcome::Completed, 1'000, true));
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown->find("account:carol"), std::string::npos) << "an unknown peer is not shown by id";
}

TEST_F(CallControlFamilyTest, OnlyAMissedCallIsNews) {
  const SystemControlFamily family = MakeCallControlFamily({});
  EXPECT_FALSE(family.quiet(Summary("account:bob", CallOutcome::Missed, std::nullopt)));
  EXPECT_FALSE(family.quiet(Summary("account:bob", CallOutcome::Cancelled, std::nullopt)));
  EXPECT_TRUE(family.quiet(Summary("account:bob", CallOutcome::Completed, 5'000)));
  EXPECT_TRUE(family.quiet(Summary(kLocalSelfContactId, CallOutcome::Missed, std::nullopt))) << "ours: not news";
  EXPECT_TRUE(family.quiet(Summary("account:bob", CallOutcome::Missed, std::nullopt, true))) << "group: not news";
}

TEST_F(CallControlFamilyTest, HandsSignalingToTheCallStack) {
  bool live = true;
  int received = 0;
  CallControlFamilyPorts ports;
  ports.receive = [&](const ThreadMessage&, const SystemControlContext& context) {
    EXPECT_EQ(context.sender_identity, "account:bob");
    ++received;
  };
  ports.has_active_local_call = [&]() { return live; };
  SystemControlFamily family = MakeCallControlFamily(ports);
  ThreadMessage invite = Message(CallControlType::CallInvite, "account:bob");
  EXPECT_TRUE(family.owns(invite));
  ASSERT_TRUE(family.apply(invite, SystemControlContext{"account:bob", std::nullopt, std::nullopt}));
  EXPECT_EQ(received, 1);
  ThreadMessage summary = Summary("account:bob", CallOutcome::Completed, 1'000);
  ASSERT_TRUE(family.apply(summary, SystemControlContext{"account:bob", std::nullopt, std::nullopt}));
  EXPECT_EQ(received, 1) << "a summary is history, not signaling";
  EXPECT_TRUE(family.live());
}

} // namespace
} // namespace pbr
