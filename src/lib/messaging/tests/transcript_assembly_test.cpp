// Which rows a transcript shows and what folds onto them (the GUI draws them).
#include "lib/messaging/TranscriptAssembly.h"

#include "lib/messaging/ReactionTypes.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

ThreadMessage Msg(const std::string& id, const std::string& sender, const std::string& text, int64_t order) {
  ThreadMessage m;
  m.id = id;
  m.thread_id = "t";
  m.sender_contact_id = sender;
  m.content_type = ChatContentType::Text;
  m.text = text;
  m.display_order = order;
  return m;
}

ThreadMessage Reaction(const std::string& id, const std::string& sender, const std::string& target,
                       const std::string& emoji, int64_t order, const char* type = kAnnotationTypeReaction) {
  ThreadMessage m = Msg(id, sender, emoji, order);
  m.content_type = ChatContentType::Annotation;
  m.target_message_id = target;
  m.payload_json = BuildReactionPayloadJson(type, target, emoji);
  return m;
}

std::vector<std::string> Ids(const std::vector<TranscriptRow>& rows) {
  std::vector<std::string> out;
  for (const auto& r : rows) {
    out.push_back(r.message.id);
  }
  return out;
}

TEST(TranscriptAssemblyTest, ReactionsFoldOntoTheirTarget) {
  const std::string thumbs = "\xF0\x9F\x91\x8D";
  const std::vector<ThreadMessage> messages = {
      Msg("m1", "account:p", "hi", 1),
      Reaction("r1", "account:p", "m1", thumbs, 2),
      Reaction("r2", kLocalSelfContactId, "m1", thumbs, 3),
      Reaction("r3", "account:q", "m1", thumbs, 4),
      Reaction("r4", "account:q", "m1", thumbs, 5, kAnnotationTypeReactionClear),  // q takes it back
      Reaction("r5", "account:p", "gone", thumbs, 6),                             // target not here
  };
  const auto rows = AssembleTranscript(messages, nullptr);
  EXPECT_EQ(Ids(rows), (std::vector<std::string>{"m1", "r5"}));
  ASSERT_EQ(rows[0].reactions.size(), 1u);
  EXPECT_EQ(rows[0].reactions[0].emoji, thumbs);
  EXPECT_EQ(rows[0].reactions[0].count, 2u) << "each person's latest; a cleared one is gone";
  EXPECT_TRUE(rows[0].reactions[0].mine);
  EXPECT_TRUE(rows[1].orphan_annotation) << "unlinked, in its place in time";
}

TEST(TranscriptAssemblyTest, WhatNeverShows) {
  ThreadMessage deleted = Msg("m2", "account:p", "", 2);
  deleted.content_type = ChatContentType::Deleted;
  ThreadMessage lane = Msg("c1", "account:p", "signal", 4);
  lane.content_type = ChatContentType::System;
  lane.payload_json = "lane";
  ThreadMessage line = Msg("c2", "account:p", "summary", 5);
  line.content_type = ChatContentType::System;
  line.payload_json = "line";
  ThreadMessage retract = Reaction("x1", "account:p", "m1", "", 6, kAnnotationTypeRetract);
  SystemControlRegistry controls;
  SystemControlFamily family;
  family.name = "call";
  family.owns = [](const ThreadMessage& m) { return m.payload_json == "lane" || m.payload_json == "line"; };
  family.on_lane = [](const ThreadMessage& m) { return m.payload_json == "lane"; };
  controls.Bind(family);
  const auto rows = AssembleTranscript(
      {Msg("m1", "account:p", "a", 1), deleted, Reaction("r1", "account:p", "m2", "x", 3), lane, line, retract},
      &controls);
  EXPECT_EQ(Ids(rows), (std::vector<std::string>{"m1", "m2", "c2"}))
      << "the tombstone shows; what hung off it, lane signaling and a retract do not";
  EXPECT_TRUE(rows[2].control);
  EXPECT_FALSE(rows[0].control);
}

TEST(TranscriptAssemblyTest, AnAiAnswerCarriesTheQuestion) {
  const auto rows = AssembleTranscript({Msg("q", kLocalSelfContactId, "@ai what is up", 1),
                                        Msg("a", kAiAssistantContactId, "sky", 2),
                                        Msg("a2", kAiAssistantContactId, "more", 3)},
                                       nullptr);
  ASSERT_EQ(rows.size(), 3u);
  ASSERT_TRUE(rows[1].asked_question);
  EXPECT_NE(rows[1].asked_question->find("what is up"), std::string::npos);
  EXPECT_FALSE(rows[2].asked_question) << "once";
}

} // namespace
} // namespace pbr
