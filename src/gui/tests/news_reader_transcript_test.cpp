// News items read in an AI chat (DEV/PLAN-news-inline-digest.md): the request and the digest are stored as hidden
// messages of the thread. They are never drawn (also after a restart), never move the chat in the list or change its
// preview or unread count, stay in what the model is sent, and the list item stays read. A chat with a person never
// gets a news item, whatever its text.
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "feature/presenters/NewsReader.h"
#include "gui/chat/ChatAnswer.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/storage/SqliteThreadStore.h"
#include "lib/ui/ChatFormHelper.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace pbr {
namespace {

const char* kList =
    "1. [Rates rise](https://n.example/a?utm_source=brief \"pp:news\")\n"
    "2. [Oil falls](https://n.example/b \"pp:news\")";

ThreadMessage Message(const std::string& thread, const std::string& id, const std::string& sender,
                      const std::string& text, const int64_t timestamp, const char* generation = nullptr) {
  ThreadMessage message;
  message.id = id;
  message.thread_id = thread;
  message.sender_contact_id = sender;
  message.text = text;
  message.timestamp = timestamp;
  message.transport = MessageTransport::Local;
  if (generation) {
    message.generation = generation;
  }
  return message;
}

std::vector<std::string> Ids(const std::vector<MessageDisplayRow>& rows) {
  std::vector<std::string> ids;
  for (const MessageDisplayRow& row : rows) {
    ids.push_back(row.message_id.c_str());
  }
  return ids;
}

struct Inbox {
  explicit Inbox(const std::filesystem::path& dir)
      : store(dir.string()),
        contacts(dir.string()),
        shadows(directory),
        labels(contacts, PeerDisplaySourcesFrom(shadows)),
        inbox(store, contacts, labels, &shadows) {}
  SqliteThreadStore store;
  ContactsStore contacts;
  MockDirectoryClient directory;
  DirectoryShadowCache shadows;
  PeerDisplayResolver labels;
  InboxController inbox;

  std::vector<MessageDisplayRow> Rows(const std::string& thread) {
    return RenderTranscript(inbox.BuildTranscript(thread, std::nullopt, std::nullopt), inbox.Facts());
  }
};

TEST(NewsReaderTranscriptTest, HiddenNewsMessagesAreNeverDrawnAndStayInTheModelHistory) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_browser_news_reader_transcript_test";
  std::filesystem::remove_all(dir);
  {
    Inbox chat(dir);
    ASSERT_TRUE(chat.store.SetDek(ByteVector(32, 0xa5)));
    Thread thread;
    thread.id = "ai-1";
    thread.kind = ThreadKind::Ai;
    thread.title = "今天有什么新闻";
    ASSERT_TRUE(chat.store.UpsertThread(thread));
    ASSERT_TRUE(chat.store.AppendMessage(Message("ai-1", "q1", kLocalSelfContactId, "今天有什么新闻", 1000)));
    ThreadMessage list = Message("ai-1", "list", kAiAssistantContactId, kList, 2000);
    const ChatAnswerRml rendered = BuildMarkdownAnswer(kList, /*news_links=*/true);
    list.content_rml =
        "<div class=\"bubble bubble-assistant\">" + InjectEntryPlaceholders(rendered.rml, "list") + "</div>";
    ASSERT_TRUE(chat.store.AppendMessage(list));
    const auto before = chat.store.GetThread("ai-1");
    ASSERT_TRUE(before && *before);

    // One item read, one whose article could not be read.
    ASSERT_TRUE(chat.store.AppendMessage(Message("ai-1", "r1", kLocalSelfContactId,
                                                 "https://n.example/a?utm_source=brief\n这篇讲了什么", 3000,
                                                 kNewsRequestGeneration)));
    ASSERT_TRUE(chat.store.AppendMessage(
        Message("ai-1", "d1", kAiAssistantContactId, "来源：n.example\n\n文章说……", 4000, kNewsDigestGeneration)));
    ASSERT_TRUE(chat.store.AppendMessage(
        Message("ai-1", "r2", kLocalSelfContactId, "https://n.example/b\n这篇讲了什么", 5000, kNewsRequestGeneration)));
    ASSERT_TRUE(chat.store.AppendMessage(
        Message("ai-1", "f2", kAiAssistantContactId, "这篇暂时取不到内容。", 6000, kNewsFailedGeneration)));

    // The chat's row in the list: same place, same preview, nothing unread, same name.
    const auto after = chat.store.GetThread("ai-1");
    ASSERT_TRUE(after && *after);
    EXPECT_EQ((*after)->updated_at, (*before)->updated_at);
    EXPECT_EQ((*after)->preview, (*before)->preview);
    EXPECT_EQ((*after)->unread_count, (*before)->unread_count);
    EXPECT_EQ((*after)->title, "今天有什么新闻");

    const auto rows = chat.Rows("ai-1");
    EXPECT_EQ(Ids(rows), (std::vector<std::string>{"q1", "list"}));
    for (const MessageDisplayRow& row : rows) {
      EXPECT_EQ(std::string(row.content_rml.c_str()).find("这篇讲了什么"), std::string::npos);
    }

    // The model is still sent what was read, so a follow-up knows it.
    auto context = chat.store.GetMessagesForContext("ai-1", DefaultContextBudget());
    ASSERT_TRUE(context);
    std::vector<std::string> sent;
    for (const ThreadMessage& message : *context) {
      sent.push_back(message.id);
    }
    for (const char* id : {"r1", "d1", "r2", "f2"}) {
      EXPECT_NE(std::find(sent.begin(), sent.end(), id), sent.end()) << id;
    }
  }
  {
    // After a restart: still not drawn, the first item still read (its digest opens without a request), the second
    // not, and the newest message shown is the list, not a question waiting for its answer.
    Inbox chat(dir);
    ASSERT_TRUE(chat.store.SetDek(ByteVector(32, 0xa5)));
    EXPECT_EQ(Ids(chat.Rows("ai-1")), (std::vector<std::string>{"q1", "list"}));
    auto page = chat.store.GetMessagesPage("ai-1", std::nullopt, 100);
    ASSERT_TRUE(page);
    const NewsReads reads = ScanNewsReads(*page);
    const NewsReads::Digest* digest = reads.Find("https://n.example/a?utm_source=brief");
    ASSERT_NE(digest, nullptr);
    EXPECT_EQ(digest->message_id, "d1");
    EXPECT_EQ(digest->text, "来源：n.example\n\n文章说……");
    EXPECT_FALSE(reads.IsRead("https://n.example/b"));
    const ThreadMessage* shown = NewestShownMessage(*page);
    ASSERT_NE(shown, nullptr);
    EXPECT_EQ(shown->id, "list");

    // The stored list greys the read item from its own text.
    const std::string list_rml = chat.Rows("ai-1")[1].content_rml.c_str();
    const std::string marked = MarkNewsLinksRead(list_rml, RecoverChatLinks(kList, /*news_links=*/true),
                                                 [&reads](const std::string& url) { return reads.IsRead(url); });
    EXPECT_NE(marked.find("news-link news-read\" data-event-click=\"open_chat_link('list', 0)"), std::string::npos)
        << marked;
    EXPECT_EQ(marked.find("news-read\" data-event-click=\"open_chat_link('list', 1)"), std::string::npos);
  }
  std::filesystem::remove_all(dir);
}

// A person's message that carries the marker is drawn as their chat's links always are (asked before opening), never
// as a news item.
TEST(NewsReaderTranscriptTest, APeerMessageWithTheNewsTitleNeverOpensTheReader) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_browser_news_reader_peer_test";
  std::filesystem::remove_all(dir);
  {
    Inbox chat(dir);
    ASSERT_TRUE(chat.store.SetDek(ByteVector(32, 0xa5)));
    Thread thread;
    thread.id = "dm-1";
    thread.kind = ThreadKind::Direct;
    thread.participant_contact_ids = {"contact-1"};
    ASSERT_TRUE(chat.store.UpsertThread(thread));
    ASSERT_TRUE(chat.store.AppendMessage(
        Message("dm-1", "p1", "contact-1", "look [x](https://y.example/a \"pp:news\")", 1000)));
    const auto rows = chat.Rows("dm-1");
    ASSERT_EQ(rows.size(), 1u);
    const std::string rml = rows[0].content_rml.c_str();
    EXPECT_NE(rml.find("open_message_link('p1', 0)"), std::string::npos) << rml;
    EXPECT_EQ(rml.find("news-link"), std::string::npos);
    EXPECT_EQ(rml.find("open_chat_link"), std::string::npos);
  }
  std::filesystem::remove_all(dir);
}

} // namespace
} // namespace pbr
