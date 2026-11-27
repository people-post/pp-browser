// The transcript's side of the picture being sent (DEV/PLAN-picture-upload-bubble.md, step 6). A ChatController cannot
// be built in a unit test (it needs a live ConversationsFacade / hub; see composer_voice_markup_test.cpp), so the
// behaviour tests here drive what it delegates to — PendingUploadRows with the real RenderTranscript and the real
// ChatTranscriptScroller — in the order ChatController::SyncDisplayFromThread runs them. The PendingPictureSourcePin
// tests at the end only read ChatController's source to pin that order and its wiring; they run none of it, so the
// controller's own callbacks (a real send ending, cancel then success, the lock in a running app) stay device checks.
#include "common/Utilities.h"
#include "common/chat/MessagingLimits.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/chat/PendingUploadRows.h"
#include "gui/chat/TranscriptRml.h"

#include <gtest/gtest.h>
#include <regex>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kMinute = 60 * 1000;
constexpr int64_t kDay = 24 * 60 * kMinute;

TranscriptRow Text(const std::string& id, const std::string& sender, const int64_t at_ms, const int64_t order) {
  TranscriptRow row;
  row.message.id = id;
  row.message.thread_id = "t";
  row.message.sender_contact_id = sender;
  row.message.text = "hi " + id;
  row.message.timestamp = at_ms;
  row.message.display_order = order;
  return row;
}

/** The real row the send writes: our picture, now (drawn as a file card here: the facts have no attachment view). */
TranscriptRow SentPicture(const int64_t order) {
  TranscriptRow row = Text("sent-1", kLocalSelfContactId, util::NowUnixMs(), order);
  row.message.content_type = ChatContentType::Attachment;
  row.message.text = "photo.jpg";
  return row;
}

/** The open chat's transcript as ChatController holds it: its state, scroller, the pending rows and the store. */
struct Transcript {
  ui::Context* context = nullptr;
  bool ready = true;
  ChatScreenState chat;
  std::vector<std::string> removed;
  PendingUploadRows pending{[this](const std::string& path) { removed.push_back(path); }};
  ChatTranscriptScroller scroller{
      context, {chat.show_jump_to_latest, chat.jump_to_latest_count, chat.messages, chat.has_turns}, ready};
  std::vector<TranscriptRow> store;
  std::string open = "t";
  int dirtied = 0;

  Transcript() {
    // ChatController::BuildMessageRows: the store's window drawn through the pending rows.
    scroller.SetRowBuilder([this](const std::string& thread_id, std::optional<int64_t>, std::optional<int64_t> newest) {
      std::vector<TranscriptRow> entries = thread_id == "t" ? store : std::vector<TranscriptRow>{};
      return pending.Render(std::move(entries), TranscriptFacts{}, std::nullopt, thread_id, !newest.has_value());
    });
    // ChatController's SetDirtyTurns: paging and jumps put the row back.
    scroller.SetDirtyTurns([this]() {
      if (pending.KeepAtTail(chat.messages, open, !scroller.LoadedMaxDisplayOrder())) {
        chat.has_turns = true;
      }
      ++dirtied;
    });
  }

  /** ChatController::SyncDisplayFromThread's row steps, in its order. */
  void Sync() {
    const bool thread_changed = scroller.BeginDisplaySync(open, /*first_screen_messages=*/0);
    const auto [prev_tail_id, prev_count] = PendingUploadRows::RealTail(chat.messages);
    chat.messages = scroller.Rows(open, scroller.LoadedMinDisplayOrder(), scroller.LoadedMaxDisplayOrder());
    chat.has_turns = !chat.messages.empty();
    scroller.EndDisplaySync(thread_changed, prev_tail_id, prev_count);
    if (pending.KeepAtTail(chat.messages, open, !scroller.LoadedMaxDisplayOrder())) {
      chat.has_turns = true;
    }
  }

  void StartPicture() { pending.Apply({StartPendingUpload(1, "t", PicturePixels{4000, 3000}), {}}); }

  std::vector<std::string> Ids() const {
    std::vector<std::string> ids;
    for (const MessageDisplayRow& row : chat.messages) {
      ids.emplace_back(row.message_id.c_str());
    }
    return ids;
  }
  const MessageDisplayRow* Find(const std::string& id) const {
    for (const MessageDisplayRow& row : chat.messages) {
      if (row.message_id == id.c_str()) {
        return &row;
      }
    }
    return nullptr;
  }
};

TEST(PendingUploadRowsTest, TheRowStaysAtTheEndThroughEveryRebuild) {
  Transcript t;
  t.store = {Text("a", "contact-1", util::NowUnixMs() - 3 * kMinute, 1)};
  t.StartPicture();
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "pending-upload-1"}));
  // A message arrives and the chat is drawn again: the picture being sent stays last.
  t.store.push_back(Text("b", "contact-1", util::NowUnixMs() - kMinute, 2));
  t.Sync();
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "b", "pending-upload-1"}));
  // A path that rebuilds the rows without the display sync (back to the latest) puts it back too.
  t.scroller.OnJumpToLatest();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "b", "pending-upload-1"}));
  EXPECT_TRUE(t.chat.has_turns);
}

TEST(PendingUploadRowsTest, SentLeavesOnlyTheRealRowInTheSameRebuild) {
  Transcript t;
  t.store = {Text("a", "contact-1", util::NowUnixMs() - 3 * kMinute, 1)};
  t.StartPicture();
  t.Sync();
  ASSERT_NE(t.Find("pending-upload-1"), nullptr);
  // The send's callback (ChatController::StartAttachmentUpload): the message is stored, the row taken back without a
  // redraw, then the one display sync of that task.
  t.store.push_back(SentPicture(2));
  const bool changed =
      t.pending.Apply(OnUploadSendFinished(t.pending.Upload(), UploadSendOutcome::Sent)); // redraw=false there
  EXPECT_TRUE(changed);
  ASSERT_NE(t.Find("pending-upload-1"), nullptr) << "nothing redrawn between the two steps";
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "sent-1"}));
  EXPECT_FALSE(t.pending.Row());
}

// The send stores its message (and the transcript is redrawn for it) a moment BEFORE its callback takes the bubble off
// the queue: in that redraw the picture must be one row, the real one, not the bubble and the real row together. The
// row count of the message stays 1 across the whole refresh sequence.
TEST(PendingUploadRowsTest, TheBubbleBecomesTheSentRowWithoutAFrameShowingBoth) {
  Transcript t;
  t.store = {Text("a", "contact-1", util::NowUnixMs() - 3 * kMinute, 1)};
  t.StartPicture();
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "pending-upload-1"})) << "uploading: the bubble is the one row";
  // The message is stored; the messages-changed redraw runs before the send's callback.
  t.store.push_back(SentPicture(2));
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "sent-1"})) << "one row for the picture: the real one";
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "sent-1"}));
  // Then the callback: the item leaves the queue and the one display sync follows.
  EXPECT_TRUE(t.pending.Apply(OnUploadSendFinished(t.pending.Upload(), UploadSendOutcome::Sent)));
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "sent-1"}));
  EXPECT_FALSE(t.pending.Row());
}

// Not every real attachment is "the" bubble: a failed bubble (kept for Retry) and an older message do not match, and two
// pictures in a batch match one real row each, in order.
TEST(PendingUploadRowsTest, OnlyAnUploadingBubbleIsMatchedToARealRowStoredAfterIt) {
  Transcript t;
  TranscriptRow older = SentPicture(1);
  older.message.id = "older";
  older.message.timestamp = util::NowUnixMs() - 5 * kMinute;
  t.store = {older};
  t.StartPicture();
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"older", "pending-upload-1"})) << "an older picture is not this one";
  // A failed bubble stays, whatever else is stored.
  PendingUpload failed = t.pending.Upload();
  failed.state = PendingUploadState::Failed;
  t.pending.Apply({failed, {}});
  t.store.push_back(SentPicture(2));
  t.Sync();
  EXPECT_NE(t.Find("pending-upload-1"), nullptr) << "a failed bubble stays for Retry";
  EXPECT_NE(t.Find("sent-1"), nullptr);
}

// Text typed and sent while the picture uploads (the input stays on with a bubble): it lands above the bubble, which
// stays last until its own send ends.
TEST(PendingUploadRowsTest, TextSentWhileThePictureUploadsLandsAboveTheBubble) {
  Transcript t;
  t.store = {Text("a", "contact-1", util::NowUnixMs() - 3 * kMinute, 1)};
  t.StartPicture();
  t.Sync();
  t.store.push_back(Text("typed", kLocalSelfContactId, util::NowUnixMs(), 2));
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "typed", "pending-upload-1"}));
  t.store.push_back(SentPicture(3));
  t.pending.Apply(OnUploadSendFinished(t.pending.Upload(), UploadSendOutcome::Sent));
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "typed", "sent-1"}));
}

// The lock deletes the thumbnail: the kept row loses its picture too, so a rebuild that puts it back (paging, a jump)
// shows the grey placeholder, never the deleted file.
TEST(PendingUploadRowsTest, AfterTheLockTheKeptRowNoLongerPointsAtTheDeletedThumbnail) {
  Transcript t;
  t.StartPicture();
  t.pending.Apply(OnUploadThumbnail(t.pending.Upload(), "/p/upload-thumb-9.jpg", /*locked=*/false));
  t.Sync();
  ASSERT_NE(std::string(t.Find("pending-upload-1")->content_rml.c_str()).find("upload-thumb-9"), std::string::npos);
  t.pending.Apply(OnAppLocked(t.pending.Upload())); // as ChatController::Update does, without a redraw
  EXPECT_EQ(t.removed, (std::vector<std::string>{"/p/upload-thumb-9.jpg"}));
  ASSERT_TRUE(t.pending.Row());
  const std::string kept = t.pending.Row()->content_rml.c_str();
  EXPECT_EQ(kept.find("upload-thumb-9"), std::string::npos);
  EXPECT_EQ(kept.find("<img"), std::string::npos);
  EXPECT_NE(kept.find("chat-pending-disc"), std::string::npos) << "the disc stays";
  // The row shown is updated in place, and a rebuild that does not draw again puts back the clean row.
  EXPECT_TRUE(t.pending.RefreshTail(t.chat.messages));
  EXPECT_EQ(std::string(t.chat.messages.back().content_rml.c_str()), kept);
  t.chat.messages.pop_back();
  ASSERT_TRUE(t.pending.KeepAtTail(t.chat.messages, "t", true));
  EXPECT_EQ(std::string(t.chat.messages.back().content_rml.c_str()).find("upload-thumb-9"), std::string::npos);
  // And drawn again from scratch it is the same placeholder markup.
  t.Sync();
  EXPECT_EQ(std::string(t.Find("pending-upload-1")->content_rml.c_str()), kept);
}

TEST(PendingUploadRowsTest, TheFirstPictureOfTheDayHasItsDateStripAndTheRowsAroundItAsTheRealOneWill) {
  Transcript t;
  const int64_t now = util::NowUnixMs();
  t.store = {Text("old", kLocalSelfContactId, now - 2 * kDay, 1)};
  t.StartPicture();
  t.Sync();
  const MessageDisplayRow* pending = t.Find("pending-upload-1");
  ASSERT_NE(pending, nullptr);
  EXPECT_FALSE(std::string(pending->date_label.c_str()).empty()) << "the day's strip is there while it uploads";
  const MessageDisplayRow pending_copy = *pending;
  const MessageDisplayRow before_with_pending = *t.Find("old");

  t.store.push_back(SentPicture(2));
  t.pending.Apply(OnUploadSendFinished(t.pending.Upload(), UploadSendOutcome::Sent));
  t.Sync();
  const MessageDisplayRow* real = t.Find("sent-1");
  ASSERT_NE(real, nullptr);
  EXPECT_EQ(std::string(real->date_label.c_str()), std::string(pending_copy.date_label.c_str()));
  EXPECT_EQ(real->run_start, pending_copy.run_start);
  EXPECT_EQ(real->run_end, pending_copy.run_end);
  EXPECT_EQ(std::string(real->row_class.c_str()), std::string(pending_copy.row_class.c_str()));
  const MessageDisplayRow* before = t.Find("old");
  EXPECT_EQ(before->run_end, before_with_pending.run_end);
  EXPECT_EQ(std::string(before->content_rml.c_str()), std::string(before_with_pending.content_rml.c_str()));
}

TEST(PendingUploadRowsTest, TheRowBeforeInTheSameRunLosesItsTailAsItWillForTheRealRow) {
  Transcript t;
  t.store = {Text("mine", kLocalSelfContactId, util::NowUnixMs() - kMinute, 1)};
  t.StartPicture();
  t.Sync();
  const MessageDisplayRow* pending = t.Find("pending-upload-1");
  ASSERT_NE(pending, nullptr);
  EXPECT_FALSE(pending->run_start) << "continues our run";
  EXPECT_TRUE(pending->run_end);
  EXPECT_FALSE(t.Find("mine")->run_end);
  EXPECT_EQ(std::string(t.Find("mine")->content_rml.c_str()).find("bubble-tail"), std::string::npos);
  EXPECT_NE(std::string(pending->content_rml.c_str()).find("bubble-tail"), std::string::npos);
}

TEST(PendingUploadRowsTest, ItShowsOnlyInItsOwnChatAndIsThereAgainOnReturn) {
  Transcript t;
  t.store = {Text("a", "contact-1", util::NowUnixMs() - kMinute, 1)};
  t.StartPicture();
  t.Sync();
  ASSERT_NE(t.Find("pending-upload-1"), nullptr);
  t.open = "other";
  t.Sync();
  EXPECT_EQ(t.Find("pending-upload-1"), nullptr);
  EXPECT_TRUE(t.chat.messages.empty());
  t.open = "t";
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"a", "pending-upload-1"}));
}

TEST(PendingUploadRowsTest, CountsTailAndWindowAreTheSameWithAndWithoutIt) {
  // Over the DOM window: the trim and the window bounds must not count the picture being sent.
  std::vector<TranscriptRow> many;
  const int64_t now = util::NowUnixMs();
  for (int64_t i = 1; i <= static_cast<int64_t>(kMaxMessagesDomWindow) + 3; ++i) {
    many.push_back(Text("m" + std::to_string(i), "contact-1", now - kDay + i, i));
  }
  Transcript with;
  with.store = many;
  with.StartPicture();
  with.Sync();
  with.store.push_back(Text("late", "contact-1", now, static_cast<int64_t>(kMaxMessagesDomWindow) + 4));
  with.Sync();
  Transcript without;
  without.store = with.store;
  without.Sync();
  without.Sync();

  std::vector<std::string> real = with.Ids();
  ASSERT_EQ(real.back(), "pending-upload-1");
  real.pop_back();
  EXPECT_EQ(real, without.Ids());
  EXPECT_EQ(with.scroller.LoadedMinDisplayOrder(), without.scroller.LoadedMinDisplayOrder());
  EXPECT_EQ(with.scroller.LoadedMaxDisplayOrder(), without.scroller.LoadedMaxDisplayOrder());
  EXPECT_EQ(with.chat.show_jump_to_latest, without.chat.show_jump_to_latest);
  EXPECT_EQ(std::string(with.chat.jump_to_latest_count.c_str()),
            std::string(without.chat.jump_to_latest_count.c_str()));
  // The tail and count the display sync compares (new messages while scrolled up, search) skip it.
  const auto [tail, count] = PendingUploadRows::RealTail(with.chat.messages);
  EXPECT_EQ(tail, "late");
  EXPECT_EQ(count, without.chat.messages.size());
  // Paging reads the first row's order: the picture being sent is never first unless alone, and then has order 0
  // (nothing is before it, as with no rows at all).
  Transcript alone;
  alone.StartPicture();
  alone.Sync();
  ASSERT_EQ(alone.Ids(), (std::vector<std::string>{"pending-upload-1"}));
  EXPECT_EQ(alone.chat.messages.front().display_order, 0);
  EXPECT_EQ(PendingUploadRows::RealTail(alone.chat.messages).second, 0u);
}

TEST(PendingUploadRowsTest, OlderHistoryShownDoesNotDrawItAtTheEnd) {
  Transcript t;
  t.store = {Text("a", "contact-1", util::NowUnixMs() - kMinute, 1)};
  t.StartPicture();
  // A window that ends before the newest message (reading history): no picture at its end.
  const auto rows = t.pending.Render(t.store, TranscriptFacts{}, std::nullopt, "t", /*at_tip=*/false);
  EXPECT_EQ(rows.size(), 1u);
  EXPECT_FALSE(t.pending.Row());
  std::vector<MessageDisplayRow> shown = rows;
  EXPECT_FALSE(t.pending.KeepAtTail(shown, "t", /*at_tip=*/false));
}

// Two items (the queue's shape; one at a time is still all the controller adds today): each row has its own element
// ids, and progress written for one leaves the other's markup as it was.
TEST(PendingUploadRowsTest, TwoItemsHaveTheirOwnIdsAndProgressOfOneLeavesTheOther) {
  Transcript t;
  t.StartPicture();
  PendingUpload second = StartPendingUpload(2, "t", PicturePixels{600, 800});
  second.state = PendingUploadState::Failed;
  t.pending.Apply({second, {}});
  t.Sync();
  ASSERT_EQ(t.Ids(), (std::vector<std::string>{"pending-upload-1", "pending-upload-2"}));
  const std::string first_rml = t.Find("pending-upload-1")->content_rml.c_str();
  const std::string second_rml = t.Find("pending-upload-2")->content_rml.c_str();
  EXPECT_NE(first_rml.find(PendingRingElementId("pending-upload-1")), std::string::npos);
  EXPECT_EQ(first_rml.find("pending-upload-2"), std::string::npos);
  EXPECT_NE(second_rml.find("retry_attachment_send('pending-upload-2')"), std::string::npos);
  EXPECT_NE(PendingRingElementId("pending-upload-1"), PendingRingElementId("pending-upload-2"));

  const std::optional<std::string> due = t.pending.Progress(40);
  ASSERT_TRUE(due);
  EXPECT_EQ(*due, "pending-upload-1") << "the uploading one";
  EXPECT_EQ(t.pending.Queue().Find("pending-upload-1")->percent, 40);
  EXPECT_EQ(t.pending.Queue().Find("pending-upload-2")->percent, 0);
  t.Sync();
  EXPECT_EQ(std::string(t.Find("pending-upload-2")->content_rml.c_str()), second_rml);
  // An event by id reaches only that item; RealTail skips both.
  EXPECT_TRUE(t.pending.Apply("pending-upload-2", OnUploadDiscardTapped));
  t.Sync();
  EXPECT_EQ(t.Ids(), (std::vector<std::string>{"pending-upload-1"}));
  EXPECT_FALSE(t.pending.Apply("pending-upload-2", OnUploadRetryTapped)) << "gone: nothing";
}

class PendingPictureFilesTest : public testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("pp_pending_picture-") + testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string Thumb(const char* name) {
    const std::string path = (dir_ / name).string();
    std::ofstream(path, std::ios::binary) << "jpeg";
    return path;
  }
  std::filesystem::path dir_;
  PendingUploadRows rows_; // deletes files for real
};

TEST_F(PendingPictureFilesTest, LockingDeletesTheThumbnailFromDisk) {
  rows_.Apply({StartPendingUpload(1, "t", PicturePixels{800, 600}), {}});
  const std::string thumb = Thumb("upload-thumb-1.jpg");
  rows_.Apply(OnUploadThumbnail(rows_.Upload(), thumb, /*locked=*/false));
  ASSERT_EQ(rows_.Upload().thumb_path, thumb);
  EXPECT_TRUE(rows_.Apply(OnAppLocked(rows_.Upload())));
  EXPECT_FALSE(std::filesystem::exists(thumb));
  EXPECT_TRUE(rows_.Upload().thumb_path.empty());
  EXPECT_EQ(rows_.Upload().state, PendingUploadState::Uploading);
}

TEST_F(PendingPictureFilesTest, AThumbnailFinishedUnderTheLockIsDeletedAndNotShown) {
  rows_.Apply({StartPendingUpload(1, "t", PicturePixels{800, 600}), {}});
  const std::string thumb = Thumb("upload-thumb-2.jpg");
  rows_.Apply(OnUploadThumbnail(rows_.Upload(), thumb, /*locked=*/true));
  EXPECT_FALSE(std::filesystem::exists(thumb));
  EXPECT_TRUE(rows_.Upload().thumb_path.empty());
}

TEST_F(PendingPictureFilesTest, CancelWhilePreparingRemovesTheRowAndALateThumbnailIsDeleted) {
  rows_.Apply({StartPendingUpload(1, "t", std::nullopt), {}});
  rows_.Apply(OnUploadCancelTapped(rows_.Upload()));
  rows_.Apply(OnUploadSendFinished(rows_.Upload(), UploadSendOutcome::Cancelled)); // the send calls back
  EXPECT_FALSE(rows_.Upload().Shows());
  const std::string late = Thumb("upload-thumb-3.jpg");
  rows_.Apply(OnUploadThumbnail(rows_.Upload(), late, /*locked=*/false));
  EXPECT_FALSE(std::filesystem::exists(late));
}

// Profile reset and shutdown tear messaging down without the lock check seeing it: Drop deletes the plaintext.
TEST_F(PendingPictureFilesTest, DroppingTheBubbleDeletesTheThumbnailAndClearsTheState) {
  rows_.Apply({StartPendingUpload(1, "t", PicturePixels{800, 600}), {}});
  const std::string thumb = Thumb("upload-thumb-drop.jpg");
  rows_.Apply(OnUploadThumbnail(rows_.Upload(), thumb, false));
  rows_.Drop();
  EXPECT_FALSE(std::filesystem::exists(thumb));
  EXPECT_FALSE(rows_.Upload().Shows());
  EXPECT_FALSE(rows_.Row());
}

TEST_F(PendingPictureFilesTest, EverySendEndDeletesTheThumbnail) {
  for (const UploadSendOutcome outcome :
       {UploadSendOutcome::Sent, UploadSendOutcome::Cancelled, UploadSendOutcome::FailedNotStaged}) {
    rows_.Apply({StartPendingUpload(1, "t", PicturePixels{800, 600}), {}});
    const std::string thumb = Thumb("upload-thumb-end.jpg");
    rows_.Apply(OnUploadThumbnail(rows_.Upload(), thumb, false));
    rows_.Apply(OnUploadSendFinished(rows_.Upload(), outcome));
    EXPECT_FALSE(std::filesystem::exists(thumb)) << static_cast<int>(outcome);
  }
  // Failed with a staged copy keeps it for the bubble; giving it up deletes it.
  rows_.Apply({StartPendingUpload(2, "t", PicturePixels{800, 600}), {}});
  const std::string thumb = Thumb("upload-thumb-failed.jpg");
  rows_.Apply(OnUploadThumbnail(rows_.Upload(), thumb, false));
  rows_.Apply(OnUploadSendFinished(rows_.Upload(), UploadSendOutcome::FailedStaged));
  EXPECT_TRUE(std::filesystem::exists(thumb));
  rows_.Apply(OnUploadDiscardTapped(rows_.Upload()));
  EXPECT_FALSE(std::filesystem::exists(thumb));
}

// --- Source-order pins: ChatController's wiring read from its source (it cannot be built here). These check that the
// calls are there and in this order, not that they behave; the behaviour is covered above on PendingUploadRows. ---

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  // A Windows checkout has CRLF line ends; Body() looks for "\n}\n".
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string Controller() {
  return ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
}

/** The body of the function whose definition starts with `signature` (to its closing brace at column 0). */
std::string Body(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) {
    return {};
  }
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

TEST(PendingPictureSourcePinTest, TheDisplaySyncCountsRealRowsAndPutsThePictureBackLast) {
  const std::string body = Body(Controller(), "void ChatController::SyncDisplayFromThread(");
  ASSERT_FALSE(body.empty());
  const size_t real_tail = body.find("PendingUploadRows::RealTail(chat_.messages)");
  const size_t rows = body.find("chat_.messages = scroller_.Rows(");
  const size_t end_sync = body.find("scroller_.EndDisplaySync(");
  const size_t search = body.find("OnSearchMessagesChanged()");
  const size_t keep = body.find("pending_uploads_.KeepAtTail(");
  ASSERT_NE(real_tail, std::string::npos);
  ASSERT_NE(keep, std::string::npos);
  EXPECT_LT(real_tail, rows);
  EXPECT_LT(end_sync, keep);
  EXPECT_LT(search, keep);
}

TEST(PendingPictureSourcePinTest, RowsAreBuiltThroughThePendingRowsAtTheTip) {
  const std::string body = Body(Controller(), "std::vector<MessageDisplayRow> ChatController::BuildMessageRows(");
  EXPECT_NE(body.find("pending_uploads_.Render("), std::string::npos);
  EXPECT_NE(body.find("!newest_inclusive.has_value()"), std::string::npos);
}

TEST(PendingPictureSourcePinTest, ASentPictureIsTakenBackWithoutARedrawThenTheOneSyncDrawsTheRealRow) {
  const std::string body = Body(Controller(), "void ChatController::StartAttachmentUpload(");
  ASSERT_FALSE(body.empty());
  const size_t finished = body.find("PendingUploadStep step = OnUploadSendFinished(now, outcome);");
  ASSERT_NE(finished, std::string::npos);
  EXPECT_NE(body.find("/*redraw=*/!sent", finished), std::string::npos);
  const size_t sync = body.find("SyncDisplayFromThread();", finished);
  EXPECT_NE(sync, std::string::npos) << "the real row comes with the display sync in the same callback";
  // A failure while preparing removes the bubble and says why, unless it was cancelled.
  EXPECT_NE(body.find("UploadSendOutcome::FailedNotStaged"), std::string::npos);
  EXPECT_NE(body.find("if (!(with_bubble && cancel->load()))"), std::string::npos);
}

TEST(PendingPictureSourcePinTest, TheLockDeletesTheThumbnailOnTheExistingLockCheck) {
  const std::string body = Body(Controller(), "void ChatController::Update(");
  const size_t lock = body.find("pending_uploads_.Apply(item.row_id, OnAppLocked)");
  ASSERT_NE(lock, std::string::npos);
  EXPECT_NE(body.rfind("messaging_ready_ && !MessagingReady()", lock), std::string::npos);
  EXPECT_NE(body.find("DropThumbnail(", lock), std::string::npos);
  EXPECT_NE(body.find("pending_uploads_.RefreshTail(chat_.messages)", lock), std::string::npos);
}

TEST(PendingPictureSourcePinTest, AJumpJudgesTheTailByTheRealRows) {
  const std::string scroller = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatTranscriptScroller.cpp");
  const std::string body = Body(scroller, "ChatTranscriptScroller::JumpResult ChatTranscriptScroller::JumpToMessage(");
  EXPECT_NE(body.find("PendingUploadRows::RealTail(view_.messages).first == message_id"), std::string::npos);
}

TEST(PendingPictureSourcePinTest, ProfileResetAndShutdownDropTheBubble) {
  const std::string source = Controller();
  EXPECT_NE(Body(source, "void ChatController::OnProfileDataReset(").find("pending_uploads_.Drop()"),
            std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::Shutdown(").find("pending_uploads_.Drop()"), std::string::npos);
}

TEST(PendingPictureSourcePinTest, RetryAndDiscardAskWhoseTapItIs) {
  const std::string source = Controller();
  for (const char* handler :
       {"void ChatController::OnRetryAttachmentSend(", "void ChatController::OnDiscardAttachmentSend("}) {
    SCOPED_TRACE(handler);
    const std::string body = Body(source, handler);
    const size_t decide = body.find("FailedVoiceTakesTheTap(");
    ASSERT_NE(decide, std::string::npos);
    EXPECT_LT(decide, body.find("failed_voice_path_;")) << "decided before the voice branch reads the recording";
  }
}

TEST(PendingPictureSourcePinTest, BesideABubbleTheMicrophoneIsGreyedNotHidden) {
  const std::string body = Body(Controller(), "void ChatController::SyncComposerInputState(");
  const size_t refresh = body.find("voice_->Refresh()");
  const size_t off = body.find("const bool voice_off = AttachmentBusy() && !chat_.show_voice");
  ASSERT_NE(off, std::string::npos);
  EXPECT_LT(refresh, off) << "decided after VoiceComposer has hidden the microphone";
  EXPECT_NE(body.find("chat_.voice_entry_off = voice_off"), std::string::npos);
}

// Plan step 1 item 2a: the chip's Retry / Discard carry no id and never touch a bubble; a bubble's carry its id.
TEST(PendingPictureSourcePinTest, ChipEventsHaveNoIdAndBubbleEventsCarryOne) {
  const std::string source = Controller();
  for (const char* handler : {"void ChatController::OnRetryAttachmentSend(const std::string& item_id)",
                              "void ChatController::OnDiscardAttachmentSend(const std::string& item_id)",
                              "void ChatController::OnCancelAttachmentSend(const std::string& item_id)"}) {
    SCOPED_TRACE(handler);
    const std::string body = Body(source, handler);
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("pending_uploads_.Queue().Find(item_id)"), std::string::npos);
    EXPECT_EQ(body.find("pending_uploads_.Upload()"), std::string::npos) << "never the current item by default";
  }
  for (const char* event : {"retry_attachment_send", "discard_attachment_send", "cancel_attachment_send"}) {
    EXPECT_NE(source.find(std::string("ctor.BindEventCallback(\"") + event +
                          "\",\n                               with_item_id("),
              std::string::npos)
        << event << " reads its optional id";
  }
}

// Step 2: a video's bubble waits for its poster and a poster that comes after a cancel is deleted; a file / video
// bubble learns its name and size from the preparation. A voice recording gets a bubble too (the finished voice bubble
// with the upload disc over its play button), made at once like a file's; its file fate stays with the voice flag.
TEST(PendingPictureSourcePinTest, VideoPosterFirstLatePosterDeletedVoiceHasItsOwnBubble) {
  const std::string body = Body(Controller(), "void ChatController::StartAttachmentUpload(");
  ASSERT_FALSE(body.empty());
  EXPECT_NE(body.find("const bool with_bubble = bubble.has_value();"), std::string::npos);
  EXPECT_NE(body.find("with_bubble && *bubble == PendingUploadKind::Voice"), std::string::npos);
  EXPECT_NE(body.find("DecideVoiceSendSettle("), std::string::npos)
      << "the recording's file still follows the voice flag";
  const size_t poster = body.find("WriteVideoPosterThumbnail(path, scratch_dir)");
  const size_t prepare = body.find("PrepareOutgoingAttachment(path, limits, source, scratch_dir)");
  ASSERT_NE(poster, std::string::npos);
  EXPECT_LT(poster, prepare) << "the poster before the preparation";
  const size_t cancelled = body.find(
      "if (!pick_current || session != messaging_session_ || send_cancel_ != cancel || cancel->load()) {", poster);
  ASSERT_NE(cancelled, std::string::npos);
  EXPECT_LT(body.find("remove_poster();", cancelled), body.find("StartPendingUpload(", cancelled));
  EXPECT_NE(body.find("return OnUploadDetails(now, title, size);"), std::string::npos);
}

// Step 3 wiring (source-order pins only): every picker result is checked against its pick serial before anything is
// done with it; the picture choice, the lock and a refused pick end the pick; a refused pick says why.
TEST(PendingPictureSourcePinTest, PickResultsCarryTheirSerialAndEveryEndClosesTheOverlay) {
  const std::string source = Controller();
  const std::string attach = Body(source, "void ChatController::OnAttachFile(");
  size_t taken = 0;
  for (size_t at = attach.find("= TakePickResult(serial, paths);"); at != std::string::npos;
       at = attach.find("= TakePickResult(serial, paths);", at + 1)) {
    ++taken;
  }
  EXPECT_EQ(taken, 3u) << "the file picker's, the library's (Android) and the computer pickers' results";
  EXPECT_NE(Body(source, "void ChatController::OpenCameraCapture(").find("= TakePickResult(serial, paths);"),
            std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::ShowPictureSendChoice(").find("picks_.EndCurrent();"),
            std::string::npos);
  const std::string start = Body(source, "void ChatController::StartAttachmentUpload(");
  EXPECT_NE(start.find("UserFeedback::Fail(Tr(\"chat.attach.previous_still_sending\"))"), std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::Update(").find("AbandonPick(); // locked"), std::string::npos);
}

// Kenneth 2026-11-19: the system camera's "Use Photo" is the confirmation, so a camera picture goes straight on, sent
// smaller, with no "Send / Send original / Cancel" choice. The choice stays for the Android library pick (a computer's
// pickers go into the send-preview page since the drop / paste work).
TEST(PendingPictureSourcePinTest, ACameraPictureGoesStraightOnWithoutTheSendChoice) {
  const std::string source = Controller();
  const std::string ports = Body(source, "CaptureFlowPorts ChatController::MakeCaptureFlowPorts(");
  ASSERT_FALSE(ports.empty());
  const size_t at = ports.find("ports.ask_picture = ");
  ASSERT_NE(at, std::string::npos);
  const std::string ask = ports.substr(at, ports.find("ports.send_video", at) - at);
  EXPECT_EQ(ask.find("ShowPictureSendChoice"), std::string::npos) << "no second prompt after the camera";
  EXPECT_NE(ask.find("thread_id != ActiveThreadId()"), std::string::npos) << "the chat changed meanwhile: not sent";
  EXPECT_NE(ask.find("Tr(\"chat.attach.not_sent_left_chat\")"), std::string::npos);
  std::string squeezed;
  for (const char c : ask) {
    if (c != ' ' && c != '\n') {
      squeezed += c;
    }
  }
  EXPECT_NE(squeezed.find(
                "OutgoingPickSource::Photo,/*voice_file=*/false,thread_id,PendingUploadKind::Picture,pick_serial);"),
            std::string::npos)
      << "shrunk, to the armed chat";
  // The library pick of Android still asks.
  EXPECT_NE(Body(source, "void ChatController::OnAttachFile(").find("ShowPictureSendChoice(std::move(path), *thread);"),
            std::string::npos);
}

// B1 (source-order pins): a pick's result is only ever sent to the chat it was armed in — every picker result, the
// camera's and the picture choice hand that thread to StartAttachmentUpload, which never reads the open chat.
TEST(PendingPictureSourcePinTest, APickIsSentOnlyToTheChatItWasArmedIn) {
  const std::string source = Controller();
  const std::string start = Body(source, "void ChatController::StartAttachmentUpload(");
  ASSERT_FALSE(start.empty());
  EXPECT_NE(start.find("const std::string& thread_id = to_thread;"), std::string::npos);
  EXPECT_EQ(start.find("thread_id = ActiveThreadId()"), std::string::npos);
  EXPECT_EQ(start.find("? voice_thread_id : ActiveThreadId()"), std::string::npos);
  const std::string attach = Body(source, "void ChatController::OnAttachFile(");
  EXPECT_NE(attach.find("/*voice_file=*/false, *thread,"), std::string::npos) << "files and library videos";
  EXPECT_NE(attach.find("ShowPictureSendChoice(std::move(path), *thread);"), std::string::npos);
  const std::string choice = Body(source, "void ChatController::ShowPictureSendChoice(");
  EXPECT_NE(choice.find("thread_id != ActiveThreadId()"), std::string::npos) << "left the chat: nothing is sent";
  EXPECT_NE(choice.find("/*voice_file=*/false, thread_id, PendingUploadKind::Picture"), std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::OpenCameraCapture(").find("MakeCaptureFlowPorts(serial, *thread)"),
            std::string::npos);
  EXPECT_NE(Body(source, "std::optional<std::string> ChatController::TakePickResult(")
                .find("picks_.TakeResult(serial, ActiveThreadId(), files"),
            std::string::npos);
}

/** `source` from the first `from` to the first `to` after it. */
std::string Between(const std::string& source, const std::string& from, const std::string& to) {
  const size_t start = source.find(from);
  if (start == std::string::npos) {
    return {};
  }
  const size_t end = source.find(to, start + from.size());
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// DEV/PLAN-desktop-drop-paste.md step 2 (source pins only): on a computer, "+" opens one multi-select dialog that takes
// pictures and files together and hands them to the one intake (OpenAttachPreview); the "Send / Send original / Cancel"
// small menu is no longer asked there (Android still asks).
TEST(AttachIntakeSourcePinTest, TheComputersPickersGoOnlyThroughOpenAttachPreview) {
  const std::string source = Controller();
  EXPECT_EQ(source.find("OpenPictureSendWithPaths"), std::string::npos) << "replaced by the one intake";
  const std::string attach = Body(source, "void ChatController::OnAttachFile(");
  ASSERT_FALSE(attach.empty());
  EXPECT_EQ(attach.find("ShowOpenImageFileDialog"), std::string::npos)
      << "no single-image dialog on a computer any more";
  EXPECT_EQ(attach.find("pick_image"), std::string::npos);

  const std::string result = Between(attach, "const auto picked_for_preview", "// A picture is asked about");
  ASSERT_FALSE(result.empty());
  EXPECT_NE(result.find("= TakePickResult(serial, paths);"), std::string::npos) << "the pick serial is checked";
  EXPECT_NE(result.find("OpenAttachPreview("), std::string::npos);
  EXPECT_NE(result.find("AttachSource::Picker"), std::string::npos);
  EXPECT_EQ(result.find("ShowPictureSendChoice"), std::string::npos);
  EXPECT_EQ(result.find("StartAttachmentUpload"), std::string::npos);

  EXPECT_EQ(attach.find("pick_computer_photos"), std::string::npos) << "no picture-only computer picker any more";

  const std::string file = Between(attach, "const auto pick_file", "// A phone's chat");
  ASSERT_FALSE(file.empty());
  EXPECT_NE(file.find("Platform::IsMobile()"), std::string::npos) << "a phone keeps its own file path";
  EXPECT_NE(file.find("ShowOpenFileDialog(Backend::GetWindow(), picked_for_preview(ArmPick()),"), std::string::npos);
  EXPECT_NE(file.find("PictureSendQueue::kMaxItems"), std::string::npos) << "up to nine files at once";
  EXPECT_NE(file.find("picked(OutgoingPickSource::File, ArmPick())"), std::string::npos) << "the phone's file pick";
}

// Kenneth 2026-11-27: on a computer the one dialog already picks pictures and files together, so "+" opens it at once;
// the menu with separate "picture" and "file" entries is built for phones and tablets only.
TEST(AttachIntakeSourcePinTest, OnAComputerPlusOpensTheOneMultiSelectDialogAndNeverBuildsThePictureFileMenu) {
  const std::string attach = Body(Controller(), "void ChatController::OnAttachFile(");
  ASSERT_FALSE(attach.empty());
  const size_t computer = attach.find("if (!Platform::IsMobile()) {\n    pick_file()();\n    return;\n  }");
  ASSERT_NE(computer, std::string::npos) << "a computer goes straight to the file dialog (pick_file's computer branch)";
  // Everything that builds a menu comes after that early return, so a computer never reaches it.
  for (const char* menu :
       {"wide_actions", "std::vector<ContextMenuAction> actions;", "ShowActionsAbove(",
        "ShowActionsPopoverLeftAligned(ev->GetCurrentElement(), std::move(wide_actions)", "AttachCancelAction()"}) {
    const size_t at = attach.find(menu);
    ASSERT_NE(at, std::string::npos) << menu;
    EXPECT_GT(at, computer) << menu;
  }
  // The computer branch of pick_file: all file types, up to nine, into the one intake.
  const std::string file = Between(attach, "const auto pick_file", "// A phone's chat");
  EXPECT_NE(file.find("ShowOpenFileDialog(Backend::GetWindow(), picked_for_preview(ArmPick()),\n"
                      "                           static_cast<int>(PictureSendQueue::kMaxItems));"),
            std::string::npos);
  // No entry or string of the old split is left, nor the picture-only picker.
  EXPECT_EQ(attach.find("attach_picture"), std::string::npos);
  EXPECT_EQ(attach.find("chat.attach.picture"), std::string::npos);
  EXPECT_EQ(attach.find("pick_computer_photos"), std::string::npos);
  // Phones and tablets are untouched: the compact card, the wide (iPad) menu and the small menu still have Photos + File.
  const std::string mobile = attach.substr(computer);
  EXPECT_NE(mobile.find("{\"home_attach_photo\", Tr(\"home.attach.photo\"), nullptr, pick_photo_or_video(),"),
            std::string::npos);
  EXPECT_NE(mobile.find("{\"home_attach_file\", Tr(\"chat.attach.file\"), nullptr, pick_file(),"), std::string::npos);
  EXPECT_NE(mobile.find("{\"attach_photo\", Tr(\"chat.attach.photo\"), nullptr, pick_photo_or_video(),"),
            std::string::npos);
  EXPECT_NE(mobile.find("{\"attach_file\", Tr(\"chat.attach.file\"), nullptr, pick_file(),"), std::string::npos);
  EXPECT_NE(attach.find("AgentTurnController::HomeAttachActions("), std::string::npos) << "the compact phone card";
  EXPECT_LT(attach.find("AgentTurnController::HomeAttachActions("), computer) << "kept before the computer branch";
  // Android is the one place left that asks "send smaller / original / cancel".
  EXPECT_EQ(Controller().find("ShowPictureSendChoice(std::move(path), *thread);"),
            Controller().rfind("ShowPictureSendChoice(std::move(path), *thread);"));
}

TEST(AttachIntakeSourcePinTest, TheIntakeIsWiredToTheRealStateThePresenterTheWorkerAndTheQueue) {
  const std::string source = Controller();
  const std::string open = Body(source, "void ChatController::OpenAttachPreview(");
  ASSERT_FALSE(open.empty());
  EXPECT_NE(open.find("AttachIntake"), std::string::npos);
  const std::string state = Body(source, "AttachState ChatController::CurrentAttachState(");
  ASSERT_FALSE(state.empty());
  for (const char* part : {"chat_.editing_message", "chat_.composer_input_disabled", "InAiComposerContext()",
                           "presenter_.SendHeld(", "picture_send_.IsOpen()", "layer_over_chat", "AttachmentBusy()"}) {
    EXPECT_NE(state.find(part), std::string::npos) << part;
  }
  const std::string ports = Body(source, "AttachIntakePorts ChatController::MakeAttachIntakePorts(");
  ASSERT_FALSE(ports.empty());
  EXPECT_NE(ports.find("AppRuntime::PostWorkerNormal("), std::string::npos)
      << "the file system is asked off the UI thread";
  EXPECT_NE(ports.find("AppRuntime::PostUI("), std::string::npos);
  EXPECT_NE(ports.find("ports.release_owned"), std::string::npos);
  EXPECT_NE(ports.find("ShowToast("), std::string::npos) << "the bottom pill";
  const std::string page = Body(source, "bool ChatController::OpenAttachItems(");
  ASSERT_FALSE(page.empty());
  EXPECT_NE(ports.find("OpenAttachItems("), std::string::npos);
  EXPECT_NE(page.find("BeginPictureQueue("), std::string::npos);
  EXPECT_NE(page.find("queue->Add("), std::string::npos);
  EXPECT_NE(page.find("picture_send_.OnItemAdded("), std::string::npos);
}

TEST(AttachIntakeSourcePinTest, TheFileDialogTakesAHowManyAndTheComputerPassesItOn) {
  const std::string header = ReadAll(std::string(PP_SOURCE_DIR) + "/src/foundation/platform/NativeFileDialog.h");
  EXPECT_NE(header.find("NativeFileDialogCallback callback, int max_count = 1);"), std::string::npos);
  const std::string desktop =
      ReadAll(std::string(PP_SOURCE_DIR) + "/src/foundation/platform/NativeFileDialog_Default.cpp");
  const std::string fn = Body(desktop, "void ShowOpenFileDialog(");
  EXPECT_NE(fn.find("max_count > 1"), std::string::npos) << "allow_many";
  const std::string ios = ReadAll(std::string(PP_SOURCE_DIR) + "/src/foundation/platform/NativeFileDialog_Ios.mm");
  const std::string ios_fn = Body(ios, "void ShowOpenFileDialog(");
  EXPECT_NE(ios_fn.find("const int"), std::string::npos) << "the iOS picker takes the argument and ignores it";
}

// S1 / S5 / S4 (source-order pins): every pick end also ends preparing; a stale video poster builds nothing; a refused
// recording is kept, not deleted; Retry is refused while recording or preparing.
TEST(PendingPictureSourcePinTest, PickEndsEndPreparingStalePostersBuildNothingRecordingsAreKept) {
  const std::string source = Controller();
  const std::string abandon = Body(source, "void ChatController::AbandonPick(");
  EXPECT_NE(abandon.find("EndAttachmentPreparing();"), std::string::npos);
  for (const char* fn : {"void ChatController::OnProfileDataReset(", "void ChatController::Shutdown(",
                         "void ChatController::OnCancelPicking("}) {
    EXPECT_NE(Body(source, fn).find("AbandonPick();"), std::string::npos) << fn;
  }
  EXPECT_NE(Body(source, "void ChatController::CloseChatWith(").find("AbandonPick();"), std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::Update(").find("AbandonPick(); // locked"), std::string::npos);
  const std::string start = Body(source, "void ChatController::StartAttachmentUpload(");
  EXPECT_NE(start.find("const bool pick_current = from_picture_queue || picks_.IsCurrent(pick_serial);"),
            std::string::npos);
  EXPECT_NE(start.find("session != messaging_session_"), std::string::npos);
  const size_t refused = start.find("if (voice_file && !thread_id.empty()) {");
  ASSERT_NE(refused, std::string::npos);
  EXPECT_LT(start.find("FailVoiceKeepingRecording(reuse_row, path, thread_id);", refused),
            start.find("release_pick_(path);", refused));
  const std::string retry = Body(source, "void ChatController::OnRetryAttachmentSend(");
  EXPECT_NE(retry.find("if (AttachmentBusy() || chat_.voice_recording) {"), std::string::npos);
  EXPECT_EQ(retry.find("!item->record_id.empty() ? item->record_id : failed_record_id_"), std::string::npos)
      << "N2: no fallback for a bubble";
  EXPECT_NE(retry.find("const std::string record_id = item ? item->record_id : failed_record_id_;"), std::string::npos);
}

// Offline retries (iPhone 2026-11-11, source-order pins): a retry's end goes through OnUploadResumeFinished (the bubble
// stays Failed while its staged copy is there, cancel included); a known-offline device stops the transfer at its next
// progress tick instead of the 30 s timeout; a cancel tap right after the retry / start tap is ignored; the pill says
// "no connection" for an unanswered failure while offline.
TEST(PendingPictureSourcePinTest, OfflineRetriesStayFailedFailFastAndSayNoConnection) {
  const std::string source = Controller();
  const std::string retry = Body(source, "void ChatController::OnRetryAttachmentSend(");
  const std::string run = Body(source, "void ChatController::StartAttachmentRetry(");
  EXPECT_NE(run.find("OnUploadResumeFinished(now, sent, staged)"), std::string::npos);
  EXPECT_NE(run.find("if (!sent && !stale_chip) {"), std::string::npos) << "the copy is looked for after a cancel too";
  EXPECT_NE(run.find("bubble_tap_guard_until_ ="), std::string::npos);
  EXPECT_NE(run.find("offline_stop->load()"), std::string::npos);
  const std::string progress =
      Body(source, "std::function<bool(uint64_t done, uint64_t total)> ChatController::MakeSendProgress(");
  const size_t held = progress.find("stop = held->hold.Update(facade->DeviceOffline(), SteadyNowMs());");
  ASSERT_NE(held, std::string::npos) << "only offline held for a while stops it (OfflineHold)";
  const size_t flagged = progress.find("offline_stop->store(true);", held);
  ASSERT_NE(flagged, std::string::npos);
  const size_t stopped = progress.find("return false;", flagged);
  EXPECT_NE(stopped, std::string::npos) << "the flag is set before the transfer is told to stop";
  const std::string cancel = Body(source, "void ChatController::OnCancelAttachmentSend(");
  EXPECT_NE(cancel.find("std::chrono::steady_clock::now() < bubble_tap_guard_until_"), std::string::npos);
  const std::string start = Body(source, "void ChatController::StartAttachmentUpload(");
  EXPECT_NE(start.find("MakeSendProgress(offline_stop)"), std::string::npos);
  EXPECT_NE(start.find("offline_stop->load()"), std::string::npos);
  // Re-review item 4: a result dropped because its chat was left says so.
  EXPECT_NE(Body(source, "std::optional<std::string> ChatController::TakePickResult(")
                .find("Tr(\"chat.attach.not_sent_left_chat\")"),
            std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::ShowPictureSendChoice(").find("Tr(\"chat.attach.not_sent_left_chat\")"),
            std::string::npos);
}

// Plan step 0 entry list: every way a row's id reaches the store or a request ignores the picture being sent.
TEST(PendingPictureSourcePinTest, EveryRowEventIgnoresThePictureBeingSent) {
  const std::string source = Controller();
  for (const char* event : {"toggle_reaction", "open_reactors", "play_voice", "play_audio_file", "show_send_failure",
                            "jump_to_quote", "open_message_link", "open_mention", "open_sender", "open_attachment",
                            "open_picture", "download_attachment", "retry_attachment"}) {
    SCOPED_TRACE(event);
    const size_t at = source.find("\"" + std::string(event) + "\"");
    ASSERT_NE(at, std::string::npos);
    const size_t guard = source.find("IsPendingUploadRowId(", at);
    const size_t next = source.find("ctor.BindEventCallback(", at + 1);
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(guard, next) << "the guard is in this event's handler";
  }
}

TEST(PendingPictureSourcePinTest, TheLongPressMenuOffersNothingOnThePictureBeingSent) {
  const std::string menus = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatMenus.cpp");
  const std::string actions = Body(menus, "std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  EXPECT_NE(actions.find("IsPendingUploadRowId(message_id)"), std::string::npos);
  const std::string title = Body(menus, "std::string ChatMenus::MenuTitleFor(");
  EXPECT_NE(title.find("IsPendingUploadRowId(message_id)"), std::string::npos);
}

} // namespace
} // namespace pbr
