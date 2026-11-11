#include "lib/people/AvatarGlyph.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/storage/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "gui/chat/TranscriptRml.h"
#include "feature/conversations/PeerDisplaySources.h"

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
    ASSERT_EQ(RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts()).size(),
              2u);

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
    EXPECT_EQ(RenderTranscript(inbox.BuildTranscript("thread-1", std::nullopt, std::nullopt), inbox.Facts()).size(),
              2u);

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
  EXPECT_NE(rml.find("open_reactors('m\\'&quot;&lt;x&gt;')"), std::string::npos) << rml;
  EXPECT_NE(rml.find("send_chat_action('m\\'&quot;&lt;x&gt;', 0)"), std::string::npos) << rml;
  EXPECT_NE(rml.find("&quot;&gt;&lt;b&gt;<span class=\"chat-reaction-count\">2</span>"), std::string::npos)
      << "count shown, text escaped";
}

// Kenneth 2026-11-03: the reaction chips sat under the bubble in the same row, so the avatar (bottom of the row) and the
// tail (bottom of the bubble) no longer lined up. The chips now hang inside the bubble, over its bottom edge.
TEST(TranscriptRmlTest, ReactionsHangInsideTheBubbleAndFlagTheRow) {
  TranscriptRow row;
  row.message.id = "m1";
  row.message.thread_id = "t";
  row.message.sender_contact_id = "account:peer";
  row.message.content_type = ChatContentType::Text;
  row.message.text = "hi";
  row.message.timestamp = 1;
  row.reactions.push_back(TranscriptReaction{"\xF0\x9F\x91\x8D", 1, true});
  row.reactions.push_back(TranscriptReaction{"\xF0\x9F\x8E\x89", 3, false});
  TranscriptRow plain = row;
  plain.message.id = "m2";
  plain.message.timestamp = 2;
  plain.reactions.clear();
  const auto rows = RenderTranscript({row, plain}, TranscriptFacts{});
  ASSERT_EQ(rows.size(), 2u);
  const std::string rml = rows[0].content_rml.c_str();
  EXPECT_TRUE(rows[0].has_reactions);
  EXPECT_FALSE(rows[1].has_reactions);
  EXPECT_EQ(std::string(rows[0].row_class.c_str()), "message-row-peer") << "row_class is compared as a whole elsewhere";
  // Inside the bubble (its positioning box), before the bubble closes; not after it.
  const size_t bubble_at = rml.find("<div class=\"bubble bubble-assistant chat-reaction-host");
  const size_t chips_at = rml.find("chat-reaction-row chat-reaction-row--overlay");
  ASSERT_NE(bubble_at, std::string::npos) << rml;
  ASSERT_NE(chips_at, std::string::npos) << rml;
  EXPECT_GT(chips_at, bubble_at);
  EXPECT_EQ(rml.find("menu-anchor-end"), std::string::npos) << "the row's own bottom is the body's end now";
  EXPECT_NE(rml.find("chat-reaction-chip chat-reaction-chip--mine"), std::string::npos);
  EXPECT_NE(rml.find("open_reactors('m1')"), std::string::npos) << "a tap lists who reacted";
  EXPECT_EQ(rml.find("toggle_reaction"), std::string::npos) << "following a reaction is the long-press menu's";
  EXPECT_NE(rml.find("<span class=\"chat-reaction-count\">3</span>"), std::string::npos);
  EXPECT_EQ(rml.find("<span class=\"chat-reaction-count\">1</span>"), std::string::npos) << "no count for one";
}

TEST(TranscriptRmlTest, ReactionsWithoutABubbleFollowTheLine) {
  TranscriptRow row;
  row.message.id = "s1";
  row.message.thread_id = "t";
  row.message.sender_contact_id = "account:peer";
  row.message.content_type = ChatContentType::System;
  row.message.text = "Alice joined";
  row.message.timestamp = 1;
  row.reactions.push_back(TranscriptReaction{"\xF0\x9F\x91\x8D", 1, false});
  const auto rows = RenderTranscript({row}, TranscriptFacts{});
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  EXPECT_EQ(rml.find("chat-reaction-row--overlay"), std::string::npos) << rml;
  EXPECT_NE(rml.find("<div class=\"chat-reaction-row\" menu-anchor-end=\"\">"), std::string::npos) << rml;
  EXPECT_GT(rml.find("chat-reaction-row"), rml.find("chat-system-line"));
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
  EXPECT_NE(
      std::string(rows[0].content_rml.c_str()).find("chat-quote--asked\"><p class=\"chat-quote-text\">what is it"),
      std::string::npos);
  EXPECT_EQ(std::string(rows[0].row_class.c_str()), "message-row-ai");
  EXPECT_NE(std::string(rows[1].content_rml.c_str()).find("Peer called"), std::string::npos);
  EXPECT_TRUE(rows[1].sender_label.empty());
  EXPECT_TRUE(rows[1].time_label.empty());
}

// Phone chat window: date strips, runs, the group name and the time inside the bubble.
namespace phone_rows {
// 2023-11-15 12:00 UTC: the same local day for a few minutes in any time zone.
constexpr int64_t kNoon = 1700049600000;
constexpr int64_t kMinute = 60 * 1000;
constexpr int64_t kDay = 24 * 60 * kMinute;

TranscriptRow TextRow(const std::string& id, const std::string& sender, const int64_t at) {
  TranscriptRow row;
  row.message.id = id;
  row.message.sender_contact_id = sender;
  row.message.content_type = ChatContentType::Text;
  row.message.text = "hi " + id;
  row.message.timestamp = at;
  return row;
}

TranscriptFacts NamedFacts() {
  TranscriptFacts facts;
  facts.sender_label = [](const std::string& id) {
    return id == "account:a" ? std::string("Alice") : std::string("Bob");
  };
  facts.sender_stable_id = [](const std::string& id) { return id; };
  return facts;
}
} // namespace phone_rows

TEST(TranscriptRmlTest, ADateStripSitsOnlyWhereTheDayChanges) {
  using namespace phone_rows;
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  const auto rows = RenderTranscript({TextRow("1", "account:a", kNoon), TextRow("2", "account:a", kNoon + kMinute),
                                      TextRow("3", "account:a", kNoon + 3 * kDay)},
                                     NamedFacts());
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_FALSE(rows[0].date_label.empty()) << "the first row opens its day";
  EXPECT_TRUE(rows[1].date_label.empty());
  EXPECT_FALSE(rows[2].date_label.empty());
  EXPECT_NE(std::string(rows[0].date_label.c_str()), std::string(rows[2].date_label.c_str()));
}

TEST(TranscriptRmlTest, ASenderNameShowsOnlyOnTheFirstBubbleOfARunInAGroup) {
  using namespace phone_rows;
  MentionRoster roster; // a group chat has one
  roster.local_account = "account:me";
  const std::vector<TranscriptRow> entries = {
      TextRow("1", "account:a", kNoon), TextRow("2", "account:a", kNoon + kMinute), // same run
      TextRow("3", "account:b", kNoon + 2 * kMinute),                               // another sender
      TextRow("4", "account:b", kNoon + 12 * kMinute),                              // gap over 5 minutes: a new run
      TextRow("5", kLocalSelfContactId, kNoon + 13 * kMinute)};
  const auto rows = RenderTranscript(entries, NamedFacts(), roster);
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(std::string(rows[0].group_name.c_str()), "Alice");
  EXPECT_TRUE(rows[0].run_start);
  EXPECT_TRUE(rows[1].group_name.empty());
  EXPECT_FALSE(rows[1].run_start);
  EXPECT_EQ(std::string(rows[2].group_name.c_str()), "Bob");
  EXPECT_EQ(std::string(rows[3].group_name.c_str()), "Bob");
  EXPECT_TRUE(rows[4].group_name.empty()) << "our own bubbles carry no name";
  // The avatar column: on every received row, the avatar only beside the LAST bubble of a run; none on our own.
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_TRUE(rows[i].group_column) << i;
  }
  EXPECT_FALSE(rows[4].group_column);
  EXPECT_FALSE(rows[0].group_avatar);
  EXPECT_TRUE(rows[1].group_avatar);
  EXPECT_EQ(std::string(rows[1].group_avatar_letter.c_str()), "A");
  EXPECT_TRUE(rows[2].group_avatar) << "Bob's first run is one bubble: first and last";
  EXPECT_TRUE(rows[3].group_avatar);
  EXPECT_FALSE(rows[4].group_avatar);
  // run_end only on the last bubble of a run, and only that bubble carries the tail element.
  const bool expect_end[5] = {false, true, true, true, true};
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(rows[i].run_end, expect_end[i]) << i;
    EXPECT_EQ(std::string(rows[i].content_rml.c_str()).find("bubble-tail ") != std::string::npos, expect_end[i]) << i;
  }
  EXPECT_NE(std::string(rows[1].content_rml.c_str()).find("bubble-tail-in"), std::string::npos);
  EXPECT_NE(std::string(rows[4].content_rml.c_str()).find("bubble-tail-out"), std::string::npos);
  // One person, one colour: the same glyph the Contacts list builds from the person's id.
  EXPECT_EQ(rows[0].group_name_tone, MakeAvatarGlyph("Alice", "account:a").tone);
  // The name sits inside the bubble, on the first bubble of a run only.
  const std::string first = rows[0].content_rml.c_str();
  const size_t bubble_at = first.find("class=\"bubble bubble-assistant");
  const size_t name_at = first.find("chat-group-name");
  ASSERT_NE(bubble_at, std::string::npos);
  ASSERT_NE(name_at, std::string::npos);
  EXPECT_GT(name_at, bubble_at);
  EXPECT_TRUE(rows[0].group_name_in_bubble);
  // Kenneth 2026-11-03: a tap on a member's name opens their page.
  EXPECT_NE(first.find("data-event-click=\"open_sender('" + std::string(rows[0].message_id.c_str()) + "')\""),
            std::string::npos)
      << first;
  EXPECT_EQ(std::string(rows[1].content_rml.c_str()).find("chat-group-name"), std::string::npos);
  // A direct chat (no roster) never shows names above bubbles.
  const auto direct = RenderTranscript(entries, NamedFacts());
  for (const MessageDisplayRow& row : direct) {
    EXPECT_TRUE(row.group_name.empty());
    EXPECT_FALSE(row.group_column);
    EXPECT_FALSE(row.group_avatar);
    EXPECT_EQ(std::string(row.content_rml.c_str()).find("chat-group-name"), std::string::npos);
  }
  EXPECT_TRUE(direct[0].run_start);
  EXPECT_FALSE(direct[1].run_start);
}

TEST(TranscriptRmlTest, TheTimeSitsInsideTheBubble) {
  using namespace phone_rows;
  const auto rows = RenderTranscript({TextRow("1", "account:a", phone_rows::kNoon)}, NamedFacts());
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  const size_t bubble = rml.find("class=\"bubble ");
  const size_t time = rml.find("<div class=\"bubble-time\">");
  ASSERT_NE(time, std::string::npos) << rml;
  EXPECT_LT(bubble, time);
  EXPECT_LT(time, rml.rfind("</div>"));
  EXPECT_EQ(rml.find("bubble-time\">", time) + 13 + 5, rml.find("</div>", time)) << "HH:MM";
}

// G016: the group's own chat says who ended it; the owner's device says "You".
TEST(TranscriptRmlTest, ADissolvedGroupSaysWhoDissolvedIt) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  LocalizationService::Instance().SetPreferredLanguage("en");
  const auto line_from = [](const std::string& sender) {
    TranscriptRow row;
    row.message = GroupMembershipCodec::BuildSystemMessage(
                      "t", GroupMembershipControlType::GroupDissolved, "Group dissolved",
                      GroupMembershipCodec::EncodeGroupDissolved("group:hike", "account:alice").value(), sender)
                      .value();
    TranscriptFacts facts;
    facts.sender_label = [](const std::string& id) {
      return id == "account:alice" ? std::string("Alice") : std::string();
    };
    const auto rows = RenderTranscript({row}, facts);
    EXPECT_EQ(rows.size(), 1u);
    return rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str());
  };
  EXPECT_NE(line_from("account:alice").find("Alice dissolved this group"), std::string::npos);
  EXPECT_NE(line_from(kLocalSelfContactId).find("You dissolved this group"), std::string::npos);
  EXPECT_NE(line_from("account:stranger").find("This group was dissolved"), std::string::npos) << "no name to give";
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_NE(line_from("account:alice").find("Alice 解散了这个群"), std::string::npos);
  EXPECT_NE(line_from(kLocalSelfContactId).find("你解散了这个群"), std::string::npos);
  LocalizationService::Instance().SetPreferredLanguage("en");
}

// Card fallbacks are localized, not hard-coded English.
TEST(TranscriptRmlTest, UnsupportedAndEmptyCardsUseLocalizedText) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  const auto render = [](ChatContentType type) {
    TranscriptRow row;
    row.message.id = "u1";
    row.message.sender_contact_id = "account:peer";
    row.message.content_type = type;
    const auto rows = RenderTranscript({row}, TranscriptFacts{});
    return rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str());
  };
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_NE(render(ChatContentType::Unsupported).find("Update the app to view this message."), std::string::npos);
  EXPECT_NE(render(ChatContentType::CryptoTx).find("Transaction"), std::string::npos);
  EXPECT_NE(render(ChatContentType::Attachment).find("Attachment"), std::string::npos);
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_NE(render(ChatContentType::Unsupported).find("请更新应用以查看此消息。"), std::string::npos);
  EXPECT_NE(render(ChatContentType::CryptoTx).find("交易"), std::string::npos);
  EXPECT_NE(render(ChatContentType::Attachment).find("附件"), std::string::npos);
  LocalizationService::Instance().SetPreferredLanguage("en");
}

TEST(TranscriptRmlTest, TheGroupAvatarColumnShowsTheSendersPhotoWhenItIsCachedAndTheLetterWhenNot) {
  using namespace phone_rows;
  MentionRoster roster;
  roster.local_account = "account:me";
  TranscriptFacts facts = NamedFacts();
  facts.sender_icon = [](const std::string& id) {
    return id == "account:a" ? std::string("/profiles/x/icons/a.jpg") : std::string();
  };
  const auto rows =
      RenderTranscript({TextRow("1", "account:a", kNoon), TextRow("2", "account:b", kNoon + kMinute)}, facts, roster);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_TRUE(rows[0].group_has_icon);
  EXPECT_EQ(std::string(rows[0].group_avatar_src.c_str()), "/profiles/x/icons/a.jpg");
  EXPECT_FALSE(rows[1].group_has_icon) << "no cached photo: the letter avatar stays";
  EXPECT_TRUE(rows[1].group_avatar_src.empty());
  EXPECT_EQ(std::string(rows[1].group_avatar_letter.c_str()), "B");
}

TEST(TranscriptRmlTest, ARebuiltTranscriptPicksUpASendersNewPhoto) {
  using namespace phone_rows;
  MentionRoster roster;
  roster.local_account = "account:me";
  std::string photo = "/profiles/x/icons/icon-1.jpg";
  TranscriptFacts facts = NamedFacts();
  facts.sender_icon = [&photo](const std::string&) { return photo; };
  const std::vector<TranscriptRow> messages = {TextRow("1", "account:a", kNoon)};
  EXPECT_EQ(std::string(RenderTranscript(messages, facts, roster)[0].group_avatar_src.c_str()), photo);

  photo = "/profiles/x/icons/icon-2.jpg"; // the peer's new photo has landed under a new path
  EXPECT_EQ(std::string(RenderTranscript(messages, facts, roster)[0].group_avatar_src.c_str()),
            "/profiles/x/icons/icon-2.jpg");
}

} // namespace
} // namespace pbr
