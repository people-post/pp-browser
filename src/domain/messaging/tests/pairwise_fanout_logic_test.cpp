#include "domain/messaging/PairwiseFanoutLogic.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace pbr;

TEST(PairwiseFanoutLogicTest, SelectSkipsEmptyAndSkipIdentity) {
  const std::vector<std::string> ids = {"a", "", "b", "skip", "c"};
  const auto out = SelectPairwiseFanoutTargets(ids, "skip");
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], "a");
  EXPECT_EQ(out[1], "b");
  EXPECT_EQ(out[2], "c");
}

TEST(PairwiseFanoutLogicTest, SelectAppliesIncludePredicate) {
  const std::vector<std::string> ids = {"a", "b", "c"};
  const auto out = SelectPairwiseFanoutTargets(ids, "", [](const std::string& id) { return id != "b"; });
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], "a");
  EXPECT_EQ(out[1], "c");
}

TEST(PairwiseFanoutLogicTest, SelectCallJoinedOnly) {
  std::vector<CallParticipant> rows(3);
  rows[0].identity = "joined";
  rows[0].state = CallParticipantState::Joined;
  rows[1].identity = "ringing";
  rows[1].state = CallParticipantState::Ringing;
  rows[2].identity = "left";
  rows[2].state = CallParticipantState::Left;

  const auto out = SelectCallFanoutIdentities(rows, "", true, false);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0], "joined");
}

TEST(PairwiseFanoutLogicTest, SelectCallJoinedAndRingingInvited) {
  std::vector<CallParticipant> rows(4);
  rows[0].identity = "joined";
  rows[0].state = CallParticipantState::Joined;
  rows[1].identity = "ringing";
  rows[1].state = CallParticipantState::Ringing;
  rows[2].identity = "invited";
  rows[2].state = CallParticipantState::Invited;
  rows[3].identity = "skip-me";
  rows[3].state = CallParticipantState::Joined;

  const auto out = SelectCallFanoutIdentities(rows, "skip-me", true, true);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], "joined");
  EXPECT_EQ(out[1], "ringing");
  EXPECT_EQ(out[2], "invited");
}

TEST(PairwiseFanoutLogicTest, BestEffortContinuesAfterFailure) {
  const std::vector<std::string> targets = {"ok1", "bad", "ok2"};
  std::vector<std::string> sent;
  const auto result = FanOutPairwise(targets, PairwiseFanoutMode::BestEffort,
                                     [&](const std::string& id) -> pp::Roe<void> {
                                       if (id == "bad") {
                                         return pp::Error("nope");
                                       }
                                       sent.push_back(id);
                                       return {};
                                     });
  EXPECT_EQ(result.attempted, 3u);
  EXPECT_EQ(result.succeeded, 2u);
  ASSERT_EQ(result.failed_identities.size(), 1u);
  EXPECT_EQ(result.failed_identities[0], "bad");
  EXPECT_FALSE(result.first_error.has_value());
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0], "ok1");
  EXPECT_EQ(sent[1], "ok2");
}

TEST(PairwiseFanoutLogicTest, FailFastStopsOnFirstError) {
  const std::vector<std::string> targets = {"ok", "bad", "never"};
  std::vector<std::string> sent;
  const auto result = FanOutPairwise(targets, PairwiseFanoutMode::FailFast,
                                     [&](const std::string& id) -> pp::Roe<void> {
                                       if (id == "bad") {
                                         return pp::Error("stop");
                                       }
                                       sent.push_back(id);
                                       return {};
                                     });
  EXPECT_EQ(result.attempted, 2u);
  EXPECT_EQ(result.succeeded, 1u);
  ASSERT_TRUE(result.first_error.has_value());
  EXPECT_EQ(result.first_error->message, "stop");
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0], "ok");
}
