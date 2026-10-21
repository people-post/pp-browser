#include "domain/messaging/ReactionTypes.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/net/OrgBackendClientsImpl.h"
#include "domain/people/ContactsStore.h"
#include "feature/conversations/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/PeerDisplayResolver.h"

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
    PeerDisplayResolver labels(contacts, shadows);
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
    ASSERT_EQ(inbox.BuildDisplayRows("thread-1", std::nullopt, std::nullopt).size(), 2u);

    ASSERT_TRUE(inbox.DeleteMessageForMe("thread-1", "m2"));

    const auto rows = inbox.BuildDisplayRows("thread-1", std::nullopt, std::nullopt);
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
    EXPECT_EQ(inbox.BuildDisplayRows("thread-1", std::nullopt, std::nullopt).size(), 2u);

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
    PeerDisplayResolver labels(contacts, shadows);
    InboxController inbox(store, contacts, labels, &shadows);

    Thread thread;
    thread.id = "thread-1";
    thread.kind = ThreadKind::Direct;
    thread.participant_contact_ids = {"contact-1"};
    ASSERT_TRUE(store.UpsertThread(thread));
    ASSERT_TRUE(store.AppendMessage(Text("m1", "contact-1", "Oil news.\n[View details]https://example.com/a?x=1")));
    ASSERT_TRUE(store.AppendMessage(Text("m2", "contact-1", "plain <b>text</b>, no link")));

    const auto rows = inbox.BuildDisplayRows("thread-1", std::nullopt, std::nullopt);
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
