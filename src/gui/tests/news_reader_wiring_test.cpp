// The news reader's wiring in ChatController (DEV/PLAN-news-inline-digest.md). ChatController cannot be built in a unit
// test (a started messaging hub, the shell), so, as OpenLinkSourceTest and ChatPictureWiringTest do, its calls are read
// from the source: deleting one of them fails here. The decisions themselves are tested in NewsReaderTest,
// ChatAnswerTest, AgentTurnBusyTest and NewsReaderTranscriptTest.
#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  std::string folded;
  folded.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      continue;
    }
    folded += text[i];
  }
  return folded;
}

/** The text of a function from its definition line (`signature` starts a line) to the closing brace at column 0. */
std::string Body(const std::string& file, const std::string& signature) {
  const std::string source = ReadFile(std::string(PP_SOURCE_DIR) + "/" + file);
  const size_t at = source.find("\n" + signature);
  if (at == std::string::npos) {
    ADD_FAILURE() << signature << " not found in " << file;
    return {};
  }
  const size_t close = source.find("\n}\n", at);
  return source.substr(at + 1, close == std::string::npos ? std::string::npos : close + 2 - (at + 1));
}

constexpr const char* kController = "src/gui/chat/ChatController.cpp";

TEST(NewsReaderWiringTest, TheSheetClosesOnLockSignOutDeleteAndClear) {
  // The lock check runs in Update; sign-out and a profile reset tear the chat down.
  for (const char* signature :
       {"void ChatController::Update(", "void ChatController::Shutdown(", "void ChatController::OnProfileDataReset(",
        "void ChatController::CloseChatWith(", "void ChatController::AfterHistoryCleared("}) {
    EXPECT_NE(Body(kController, signature).find("CloseNewsReader();"), std::string::npos) << signature;
  }
}

TEST(NewsReaderWiringTest, TheEndOfAReadReachesTheReaderAndTheList) {
  EXPECT_NE(Body(kController, "void ChatController::OnAgentTurnEnded(").find("OnNewsReadEnded(thread_id)"),
            std::string::npos);
  // A read that ends with the sheet closed regreys its item at once: the rows are rebuilt and redrawn (e2e 2026-11-23:
  // the item stayed blue until something else redrew the chat).
  const std::string ended = Body(kController, "void ChatController::OnNewsReadEnded(");
  const size_t sync = ended.find("SyncDisplayFromThread();");
  ASSERT_NE(sync, std::string::npos);
  EXPECT_NE(ended.find("DirtyChatTurns();", sync), std::string::npos);
  EXPECT_NE(Body(kController, "std::vector<MessageDisplayRow> ChatController::BuildMessageRows(")
                .find("MarkReadNewsItems(rows, thread_id, news_texts)"),
            std::string::npos);
}

// The read goes through the session's own entry: never the send path that names the chat, moves it up the list or
// changes its preview.
TEST(NewsReaderWiringTest, AReadNeverNamesTheChatOrChangesItsPreview) {
  const std::string show = Body(kController, "void ChatController::ShowNewsItem(");
  EXPECT_NE(show.find("agent_.StartNewsRead("), std::string::npos);
  EXPECT_NE(show.find("NewsReadRequestText(item.url,"), std::string::npos) << "the request carries the item's URL";
  for (const char* forbidden :
       {"NameAiThreadFromFirstMessage", "UpdateSidebarPreview", "UpdatePreview", "SendUserText", "IncrementUnread"}) {
    EXPECT_EQ(show.find(forbidden), std::string::npos) << forbidden;
  }
}

// Closing the sheet mid-read only closes the sheet: the read goes on and its digest is stored (the session stores it,
// AgentSessionBriefStreamTest.ANewsReadStoresAHiddenRequestAndDigest); only the stop key stops it.
TEST(NewsReaderWiringTest, ClosingTheSheetNeverStopsTheRead) {
  for (const char* signature : {"void ChatController::OpenNewsReader(", "void ChatController::CloseNewsReader("}) {
    const std::string body = Body(kController, signature);
    EXPECT_EQ(body.find("OnStopTurn"), std::string::npos) << signature;
    EXPECT_EQ(body.find("cancel"), std::string::npos) << signature;
  }
}

TEST(NewsReaderWiringTest, TheAskAiPanelAndItsQuestionCheckIgnoreTheReader) {
  const std::string header = ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.h");
  EXPECT_NE(header.find("OpenChatLinkIn(thread_id, entry_id, link_index, /*news_reader=*/false);"), std::string::npos)
      << "a news item in the ask-AI panel opens the browser (one sheet at a time there)";
  EXPECT_NE(Body(kController, "bool ChatController::AskAiLastIsQuestion(").find("NewestShownMessage("),
            std::string::npos);
  // The panel's rows are the chat page's rows (BuildMessageRows), which draw no hidden message.
  EXPECT_NE(Body(kController, "std::vector<AskAiRow> ChatController::AskAiRecentRows(").find("BuildMessageRows("),
            std::string::npos);
}

TEST(NewsReaderWiringTest, TheDigestHasNothingToTap) {
  // The article is untrusted: no link in the digest is tappable ("Open original" is the way out).
  const std::string apply = Body(kController, "void ChatController::ApplyNewsReader(");
  EXPECT_NE(apply.find("MarkdownToRml(digest->text, MarkdownOptions{.links = false})"), std::string::npos);
  EXPECT_EQ(apply.find("chat_links_"), std::string::npos);
  // After a restart a hidden digest's links are rebuilt without news items, and only in an AI chat do they count.
  const std::string links = Body(kController, "const std::vector<MarkdownLink>* ChatController::ChatLinksOf(");
  EXPECT_NE(links.find("ThreadKind::Ai && !IsHiddenNewsMessage(**found)"), std::string::npos);
}

} // namespace
} // namespace pbr
