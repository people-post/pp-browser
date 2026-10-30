#include "lib/messaging/GroupMentions.h"
#include "lib/messaging/ReactionTypes.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "gui/chat/TranscriptRml.h"
#include "domain/peers/PeerDisplaySources.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace pbr {
namespace {

ThreadMessage Text(const std::string& id, const std::string& sender, const std::string& text) {
  ThreadMessage message;
  message.id = id;
  message.thread_id = "thread-1";
  message.sender_contact_id = sender;
  message.text = text;
  message.timestamp = 1;
  return message;
}

// "Delete for me": the content leaves the store, an id-only tombstone stays, the transcript shows one line
// in its place and nothing that hung off it (reactions).
TEST(InboxHideMessageTest, DeletedMessageBecomesATombstoneLine) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_browser_inbox_hide_test";
  std::filesystem::remove_all(dir);
  {
    SqliteThreadStore store(dir.string());
    ASSERT_TRUE(store.SetDek(ByteVector(32, 0xa5)));
    ContactsStore contacts(dir.string());
    MockDirectoryClient directory;
    DirectoryShadowCache shadows(directory);
    PeerDisplayResolver labels(contacts, PeerDisplaySourcesFrom(shadows));
    InboxController inbox(store, contacts, labels, &shadows);

    Thread thread;
    thread.id = "thread-1";
    thread.kind = ThreadKind::Direct;
    thread.participant_contact_ids = {"contact-1"};
    ASSERT_TRUE(store.UpsertThread(thread));
    ASSERT_TRUE(store.AppendMessage(Text("m1", "contact-1", "keep me")));
    ASSERT_TRUE(store.AppendMessage(Text("m2", "contact-1", "hide me")));
    ThreadMessage reaction = Text("r1", "contact-1", "\xF0\x9F\x91\x8D");
    reaction.content_type = ChatContentType::Annotation;
    reaction.target_message_id = "m2";
    reaction.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, "m2", "\xF0\x9F\x91\x8D");
    ASSERT_TRUE(store.AppendMessage(reaction));
    ASSERT_EQ(RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts()).size(), 2u);

    ASSERT_TRUE(inbox.DeleteMessageForMe("thread-1", "m2"));

    const auto rows = RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts());
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(std::string(rows[0].message_id.c_str()), "m1");
    EXPECT_EQ(std::string(rows[1].message_id.c_str()), "m2");
    // On the sender's side (a peer here), as a small grey note.
    EXPECT_NE(std::string(rows[1].content_rml.c_str()).find("bubble-assistant bubble-deleted"), std::string::npos);
    EXPECT_EQ(std::string(rows[1].content_rml.c_str()).find("hide me"), std::string::npos);
    // No orphan row for the reaction; the id is still known (dedupe), the content is gone.
    const auto has = store.HasMessageId("thread-1", "m2");
    ASSERT_TRUE(has);
    EXPECT_TRUE(*has);
    auto page = store.GetMessagesPage("thread-1", std::nullopt, 100);
    ASSERT_TRUE(page);
    const auto deleted = std::find_if(page->begin(), page->end(), [](const ThreadMessage& m) { return m.id == "m2"; });
    ASSERT_NE(deleted, page->end());
    EXPECT_EQ(deleted->content_type, ChatContentType::Deleted);
    EXPECT_TRUE(deleted->text.empty());

    EXPECT_FALSE(inbox.DeleteMessageForMe("thread-1", "no-such-message"));
    EXPECT_TRUE(inbox.DeleteMessageForMe("thread-1", "m2")); // again: nothing left to do

    // A retract waiting for its message is never shown (no orphan row).
    ThreadMessage retract = Text("x1", "contact-1", "");
    retract.content_type = ChatContentType::Annotation;
    retract.target_message_id = "not-here-yet";
    retract.payload_json = BuildReactionPayloadJson(kAnnotationTypeRetract, "not-here-yet", "");
    ASSERT_TRUE(store.AppendMessage(retract));
    EXPECT_EQ(RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts()).size(), 2u);

    // Deleting the newest message: the list preview stops showing its text.
    ASSERT_TRUE(store.AppendMessage(Text("m3", "contact-1", "newest secret")));
    ASSERT_TRUE(inbox.UpdatePreview("thread-1", "newest secret"));
    ASSERT_TRUE(inbox.DeleteMessageForMe("thread-1", "m3"));
    auto stored = store.GetThread("thread-1");
    ASSERT_TRUE(stored);
    ASSERT_TRUE(stored->has_value());
    EXPECT_EQ((*stored)->preview.find("secret"), std::string::npos);
  }
  // Windows cannot delete the database while the store above still has it open.
  std::filesystem::remove_all(dir);
}

// A shared article arrives as text: the bubble shows the label as a link and keeps the URL out of the markup.
TEST(InboxMessageLinksTest, SharedArticleShowsTheLabelAsALink) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_browser_inbox_links_test";
  std::filesystem::remove_all(dir);
  {
    SqliteThreadStore store(dir.string());
    ASSERT_TRUE(store.SetDek(ByteVector(32, 0xa5)));
    ContactsStore contacts(dir.string());
    MockDirectoryClient directory;
    DirectoryShadowCache shadows(directory);
    PeerDisplayResolver labels(contacts, PeerDisplaySourcesFrom(shadows));
    InboxController inbox(store, contacts, labels, &shadows);

    Thread thread;
    thread.id = "thread-1";
    thread.kind = ThreadKind::Direct;
    thread.participant_contact_ids = {"contact-1"};
    ASSERT_TRUE(store.UpsertThread(thread));
    ASSERT_TRUE(store.AppendMessage(Text("m1", "contact-1", "Oil news.\n[View details]https://example.com/a?x=1")));
    ASSERT_TRUE(store.AppendMessage(Text("m2", "contact-1", "plain <b>text</b>, no link")));

    const auto rows = RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts());
    ASSERT_EQ(rows.size(), 2u);
    const std::string shared = rows[0].content_rml.c_str();
    EXPECT_NE(shared.find("open_message_link('m1', 0)"), std::string::npos);
    EXPECT_NE(shared.find(">[View details]</span>"), std::string::npos);
    EXPECT_EQ(shared.find("example.com"), std::string::npos); // resolved from the stored text on click
    const std::string plain = rows[1].content_rml.c_str();
    EXPECT_EQ(plain.find("open_message_link"), std::string::npos);
    EXPECT_NE(plain.find("&lt;b&gt;"), std::string::npos); // still escaped
  }
  // Windows cannot delete the database while the store above still has it open.
  std::filesystem::remove_all(dir);
}

} // namespace
} // namespace pbr

namespace pbr {
namespace {

TEST(TranscriptRmlTest, ClickArgumentsStayInsideTheirAttribute) {
  TranscriptRow row;
  row.message.id = "m'\"<x>";
  row.message.thread_id = "t";
  row.message.sender_contact_id = "account:peer";
  row.message.content_type = ChatContentType::Text;
  row.message.text = "hi";
  row.message.chat_actions.push_back(TranscriptChatAction{});
  row.message.chat_actions.back().label = "Go";
  row.reactions.push_back(TranscriptReaction{"\"><b>", 2, false});
  const auto rows = RenderTranscript({row}, TranscriptFacts{});
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  EXPECT_EQ(rml.find("<b>"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("<x>"), std::string::npos) << rml;
  EXPECT_NE(rml.find("toggle_reaction('m\\'&quot;&lt;x&gt;', '&quot;&gt;&lt;b&gt;')"), std::string::npos) << rml;
  EXPECT_NE(rml.find("send_chat_action('m\\'&quot;&lt;x&gt;', 0)"), std::string::npos) << rml;
  EXPECT_NE(rml.find("&quot;&gt;&lt;b&gt; 2</button>"), std::string::npos) << "count shown, text escaped";
}

TEST(TranscriptRmlTest, AnAiAnswerShowsTheQuestionAndAControlLineHasNoSenderChip) {
  TranscriptRow answer;
  answer.message.id = "a1";
  answer.message.sender_contact_id = kAiAssistantContactId;
  answer.message.content_type = ChatContentType::Text;
  answer.message.text = "42";
  answer.asked_question = "what\nis it";
  TranscriptRow control;
  control.message.id = "c1";
  control.message.sender_contact_id = "account:peer";
  control.message.content_type = ChatContentType::System;
  control.message.text = "raw";
  control.message.transport = MessageTransport::Relay;
  control.control = true;
  TranscriptFacts facts;
  facts.sender_label = [](const std::string&) { return std::string("Peer"); };
  facts.control_line = [](const ThreadMessage&) { return std::optional<std::string>("Peer called"); };
  const auto rows = RenderTranscript({answer, control}, facts);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_NE(std::string(rows[0].content_rml.c_str()).find("chat-quote--asked\"><p class=\"chat-quote-text\">what is it"),
            std::string::npos);
  EXPECT_EQ(std::string(rows[0].row_class.c_str()), "message-row-ai");
  EXPECT_NE(std::string(rows[1].content_rml.c_str()).find("Peer called"), std::string::npos);
  EXPECT_TRUE(rows[1].sender_label.empty());
  EXPECT_TRUE(rows[1].time_label.empty());
}


// --- @mention highlight (G015) --------------------------------------------------------------------

TranscriptRow MentionRow(const std::string& text, const std::vector<MentionRange>& ranges) {
  TranscriptRow row;
  row.message.id = "mm1";
  row.message.thread_id = "thread-1";
  row.message.sender_contact_id = "acct-peer";
  row.message.content_type = ChatContentType::Text;
  row.message.text = text;
  MentionsPayload payload;
  payload.mentions = ranges;
  row.message.payload_json = BuildMentionsPayloadJson(text, payload);
  return row;
}

TEST(TranscriptMentionTest, HighlightsTheRangeAndMarksSelf) {
  const auto row = MentionRow("hi @Ann and @Bob!", {{"a", 3, 7}, {"b", 12, 16}});
  TranscriptFacts facts;
  facts.mentions = [](const ThreadMessage&) {
    return std::vector<TranscriptMention>{{3, 7, false}, {12, 16, true}};
  };
  const auto rows = RenderTranscript({row}, facts);
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  EXPECT_NE(rml.find("hi <span class=\"chat-mention\">@Ann</span> and "
                     "<span class=\"chat-mention chat-mention--me\">@Bob</span>!"),
            std::string::npos)
      << rml;
}

TEST(TranscriptMentionTest, OnlyAMessageThatMentionsMeTintsTheRow) {
  const auto row = MentionRow("hi @Ann and @Bob!", {{"a", 3, 7}, {"b", 12, 16}});
  TranscriptFacts facts;
  facts.mentions = [](const ThreadMessage&) {
    return std::vector<TranscriptMention>{{3, 7, false}, {12, 16, true}};
  };
  EXPECT_TRUE(RenderTranscript({row}, facts)[0].mentions_me);
  facts.mentions = [](const ThreadMessage&) { return std::vector<TranscriptMention>{{3, 7, false}}; };
  EXPECT_FALSE(RenderTranscript({row}, facts)[0].mentions_me);
}

TEST(TranscriptMentionTest, EscapesNamesAndSurroundingText) {
  const auto row = MentionRow("a&b @<i>x&y", {{"a", 4, 11}});
  TranscriptFacts facts;
  facts.mentions = [](const ThreadMessage&) { return std::vector<TranscriptMention>{{4, 11, false}}; };
  const auto rows = RenderTranscript({row}, facts);
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  EXPECT_NE(rml.find("a&amp;b <span class=\"chat-mention\">@&lt;i&gt;x&amp;y</span>"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("<i>"), std::string::npos);
}

TEST(TranscriptMentionTest, BadRangesAndNoFactsLeaveThePlainText) {
  const auto row = MentionRow("hello @x", {{"a", 6, 8}});
  EXPECT_EQ(std::string(RenderTranscript({row}, TranscriptFacts{})[0].content_rml.c_str()).find("chat-mention"),
            std::string::npos);
  TranscriptFacts facts;
  facts.mentions = [](const ThreadMessage&) { return std::vector<TranscriptMention>{{6, 99, false}}; };
  EXPECT_EQ(std::string(RenderTranscript({row}, facts)[0].content_rml.c_str()).find("chat-mention"), std::string::npos);
}

// Eligibility is checked at render time from the roster the device holds then: non-members and a non-owner's
// @all are not highlighted, and a roster that arrives late shows on the next render.
TEST(TranscriptMentionTest, RosterDecidesAndALateRosterAppearsOnReRender) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_browser_transcript_mention_test";
  std::filesystem::remove_all(dir);
  SqliteThreadStore store(dir.string());
  ASSERT_TRUE(store.SetDek(ByteVector(32, 0xa5)));
  ContactsStore contacts(dir.string());
  MockDirectoryClient directory;
  DirectoryShadowCache shadows(directory);
  PeerDisplayResolver labels(contacts, PeerDisplaySourcesFrom(shadows));
  InboxController inbox(store, contacts, labels, &shadows);

  std::vector<GroupRosterMember> roster;  // not known yet
  inbox.SetMentionContext([] { return std::string("acct-me"); }, [&roster](const std::string&) { return roster; });
  Thread thread;
  thread.id = "thread-1";
  thread.kind = ThreadKind::Group;
  thread.group_id = "group:1";
  ASSERT_TRUE(store.UpsertThread(thread));

  ThreadMessage message = Text("m1", "acct-owner", "yo @Me and @All");
  MentionsPayload payload;
  payload.mentions.push_back({"acct-me", 3, 6});
  payload.mentions.push_back({"acct-stranger", 3, 6});
  payload.all = MentionRange{"", 11, 15};
  message.payload_json = BuildMentionsPayloadJson(message.text, payload);
  ASSERT_TRUE(store.AppendMessage(message));

  const auto render = [&]() {
    const auto rows = RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts());
    return std::string(rows.at(0).content_rml.c_str());
  };
  EXPECT_EQ(render().find("chat-mention"), std::string::npos);

  GroupRosterMember owner;
  owner.member_identity = "acct-owner";
  owner.role = MemberRole::Owner;
  GroupRosterMember me;
  me.member_identity = "acct-me";
  roster = {owner, me};
  const std::string after = render();
  EXPECT_NE(after.find("<span class=\"chat-mention chat-mention--me\">@Me</span>"), std::string::npos) << after;
  EXPECT_NE(after.find("<span class=\"chat-mention chat-mention--me\">@All</span>"), std::string::npos) << after;
  EXPECT_EQ(after.find("acct-stranger"), std::string::npos);
}

TEST(TranscriptMentionTest, AllFromANonOwnerIsNotHighlighted) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_browser_transcript_mention_all_test";
  std::filesystem::remove_all(dir);
  SqliteThreadStore store(dir.string());
  ASSERT_TRUE(store.SetDek(ByteVector(32, 0xa5)));
  ContactsStore contacts(dir.string());
  MockDirectoryClient directory;
  DirectoryShadowCache shadows(directory);
  PeerDisplayResolver labels(contacts, PeerDisplaySourcesFrom(shadows));
  InboxController inbox(store, contacts, labels, &shadows);
  GroupRosterMember owner;
  owner.member_identity = "acct-owner";
  owner.role = MemberRole::Owner;
  GroupRosterMember pal;
  pal.member_identity = "acct-pal";
  inbox.SetMentionContext([] { return std::string("acct-me"); },
                          [&](const std::string&) { return std::vector<GroupRosterMember>{owner, pal}; });
  Thread thread;
  thread.id = "thread-1";
  thread.kind = ThreadKind::Group;
  thread.group_id = "group:1";
  ASSERT_TRUE(store.UpsertThread(thread));
  ThreadMessage message = Text("m1", "acct-pal", "hey @All");
  MentionsPayload payload;
  payload.all = MentionRange{"", 4, 8};
  message.payload_json = BuildMentionsPayloadJson(message.text, payload);
  ASSERT_TRUE(store.AppendMessage(message));
  const auto rows = RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts());
  EXPECT_EQ(std::string(rows.at(0).content_rml.c_str()).find("chat-mention"), std::string::npos);
}

} // namespace
} // namespace pbr
