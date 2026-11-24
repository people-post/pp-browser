// How an attachment send ends, and the one "busy" check (DEV/PLAN-picture-send-preview.md step 2). ChatController
// cannot be built in a unit test (a started messaging hub, the shell; PLAN-picture-send-preview.step1.md §6), so its
// wiring is pinned in its source: every way out of StartAttachmentUpload reports through the once-only wrapper, each
// send works on its own bubble row, every entry asks AttachmentBusy(), and a send without a picture queue goes as it
// went before. The rows' side of "each send has its own row" is checked on PendingUploadRows itself.
#include "feature/presenters/PictureSendQueue.h"
#include "gui/chat/PendingUploadRows.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string Controller() {
  return ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
}

std::string Header() {
  return ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.h");
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

size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

std::string Upload() {
  return Body(Controller(), "void ChatController::StartAttachmentUpload(");
}

/** `report` comes after `anchor` and before the first `return;` after it. */
void ExpectReportedBeforeReturn(const std::string& body, const std::string& anchor, const std::string& report) {
  SCOPED_TRACE(anchor);
  const size_t at = body.find(anchor);
  ASSERT_NE(at, std::string::npos);
  const size_t ret = body.find("return;", at);
  const size_t fired = body.find(report, at);
  ASSERT_NE(fired, std::string::npos);
  EXPECT_LT(fired, ret);
}

// --- Every way out reports once ---

TEST(AttachmentUploadExit, PictureQueueOnFinishedIsWrappedOnceAtTheEntry) {
  const std::string body = Upload();
  ASSERT_FALSE(body.empty());
  const size_t wrapped = body.find("const AttachmentSendOnce finished(std::move(on_finished));");
  ASSERT_NE(wrapped, std::string::npos);
  EXPECT_LT(wrapped, body.find("const bool busy")) << "before the first way out";
  EXPECT_EQ(Count(body, "on_finished"), 2u) << "the parameter and its one move into the wrapper";
  const std::string header = Header();
  EXPECT_NE(header.find("std::function<void(AttachmentSendOutcome)> on_finished = {},\n"), std::string::npos);
  EXPECT_NE(header.find("bool from_picture_queue = false, const std::string& reuse_row = {});"), std::string::npos);
}

TEST(AttachmentUploadExit, PictureQueueEveryExitFiresTheWrapper) {
  const std::string body = Upload();
  ASSERT_FALSE(body.empty());
  // 1-2: refused (a recording kept for Retry; anything else released unless the queue's).
  ExpectReportedBeforeReturn(body, "UserFeedback::Fail(Tr(\"chat.attach.previous_still_sending\"));", "finished.Fire(");
  // 3: let go by the overlay's Cancel before the upload.
  ExpectReportedBeforeReturn(body, "// Let go by the overlay's Cancel (another send may run now)",
                             "finished.Fire(ClassifyUploadOutcome(/*sent=*/false, /*cancelled=*/true");
  // 4: the preparation failed (unreadable, too large, cancelled while preparing).
  ExpectReportedBeforeReturn(body, "      if (!prepared) {",
                             "finished.Fire(ClassifyUploadOutcome(/*sent=*/false, cancel->load()");
  // 5: let go by the overlay's Cancel after the upload.
  ExpectReportedBeforeReturn(body, "if (send_cancel_ != cancel && !voice_file) {", "finished.Fire(");
  // 6: the send's own end (sent, cancelled, staged, pace, not staged), before either of its returns.
  const size_t ended = body.find("finished.Fire(ended);");
  ASSERT_NE(ended, std::string::npos);
  EXPECT_LT(body.find("finish_uploading();\n            const bool cancelled"), ended);
  EXPECT_LT(ended, body.find("            if (!sent) {", ended));
  EXPECT_NE(body.find("IsAttachmentRateLimited(sent.error())"), std::string::npos);
  EXPECT_EQ(Count(body, "finished.Fire("), 7u); // + a queued voice job whose bubble is gone
  // A tripwire, not a rule: a new `return;` here may be a new way out, which must report through finished.Fire (the
  // other returns are inside helpers and callbacks that are not ways out). Update the count once that is checked.
  EXPECT_EQ(Count(body, "return;"), 12u);
}

TEST(AttachmentUploadExit, PictureQueueTheWrapperRidesInEveryCallback) {
  const std::string body = Upload();
  const size_t worker = body.find("AppRuntime::PostWorkerNormal([");
  ASSERT_NE(worker, std::string::npos);
  EXPECT_LT(body.find("finished, from_picture_queue,", worker), body.find("]() {", worker));
  const size_t prepared = body.find("thumb, with_bubble, picture, bubble_row, finished, from_picture_queue,");
  EXPECT_NE(prepared, std::string::npos);
  EXPECT_NE(body.find("offline_stop, bubble_row, finished, from_picture_queue](Roe<ThreadMessage> sent)"),
            std::string::npos);
}

// --- Each send its own row ---

TEST(AttachmentUploadExit, PictureQueueEachSendCarriesItsOwnRow) {
  const std::string body = Upload();
  EXPECT_EQ(body.find("pending_uploads_.Upload()"), std::string::npos) << "never \"the first row\"";
  EXPECT_NE(body.find("*bubble_row = item.row_id;"), std::string::npos);
  EXPECT_EQ(Count(body, "*bubble_row = item.row_id;"), 2u) << "a picture's and a video's";
  EXPECT_NE(body.find("*bubble_row = file.row_id;"), std::string::npos);
  EXPECT_EQ(Count(body, "ApplyPendingUploadTo("), 6u)
      << "size, thumbnail, details, not prepared, the end, a voice end with no Retry of its own (a voice job's own "
         "failure: FailVoiceKeepingRecording)";
  EXPECT_NE(body.find("if (!pending_uploads_.Queue().Find(row_id)) {"), std::string::npos)
      << "a thumbnail for a row that is gone is deleted";
  const std::string apply = Body(Controller(), "void ChatController::ApplyPendingUploadTo(");
  EXPECT_NE(apply.find("pending_uploads_.Apply(row_id, event)"), std::string::npos);
}

TEST(AttachmentUploadExit, PictureQueueFailedBubblesGiveWayOnlyWithoutAQueue) {
  const std::string body = Upload();
  const size_t loop = body.find("for (const PendingUpload& item : std::vector<PendingUpload>(pending_uploads_");
  ASSERT_NE(loop, std::string::npos);
  const size_t skip = body.find("if (from_picture_queue || voice_file) {\n      break;", loop);
  const size_t give_way = body.find("OnOtherAttachmentStarted", loop);
  ASSERT_NE(skip, std::string::npos);
  EXPECT_LT(skip, give_way);
}

// Two bubbles at once (a queue's failed one kept, the next uploading): events by id reach their own row, while
// Upload() — what the controller used before — names the failed one.
TEST(AttachmentUploadExit, PictureQueueEventsByIdReachTheirOwnRowBesideAFailedOne) {
  std::vector<std::string> removed;
  PendingUploadRows rows([&removed](const std::string& path) { removed.push_back(path); });
  ASSERT_TRUE(rows.Apply({StartPendingUpload(1, "t", PicturePixels{40, 30}), {}}));
  const std::string first = rows.Upload().row_id;
  ASSERT_TRUE(rows.Apply(first, [](const PendingUpload& now) {
    PendingUploadStep step = OnUploadSendFinished(now, UploadSendOutcome::FailedStaged);
    step.next.record_id = "rec-1";
    return step;
  }));
  ASSERT_TRUE(rows.Apply({StartPendingUpload(2, "t", PicturePixels{40, 30}), {}}));
  const std::string second = "pending-upload-2";
  ASSERT_NE(rows.Queue().Find(second), nullptr);
  EXPECT_EQ(rows.Upload().row_id, first) << "the first row is the failed one";
  (void)rows.Apply(second, [](const PendingUpload& now) { return OnUploadThumbnail(now, "thumb-2", false); });
  EXPECT_EQ(rows.Queue().Find(second)->thumb_path, "thumb-2");
  EXPECT_TRUE(rows.Queue().Find(first)->thumb_path.empty());
  (void)rows.Apply(second, [](const PendingUpload& now) { return OnUploadSendFinished(now, UploadSendOutcome::Sent); });
  EXPECT_EQ(rows.Queue().Find(second), nullptr) << "the sent row goes";
  ASSERT_NE(rows.Queue().Find(first), nullptr) << "the failed one stays with its Retry";
  EXPECT_EQ(rows.Queue().Find(first)->state, PendingUploadState::Failed);
  EXPECT_EQ(rows.Queue().Find(first)->record_id, "rec-1");
  EXPECT_EQ(removed, (std::vector<std::string>{"thumb-2"}));
}

// --- One busy check ---

TEST(AttachmentUploadExit, PictureQueueOneBusyCheckHoldsTheSlotForTheQueue) {
  const std::string source = Controller();
  const std::string busy = Body(source, "bool ChatController::AttachmentBusy() const {");
  EXPECT_NE(busy.find("return chat_.attachment_uploading || chat_.attachment_preparing || (picture_queue_ && "
                      "picture_queue_->Active());"),
            std::string::npos);
  // The former ad-hoc checks (step1.md §7: 1-4, 6-11, 13), each now the predicate.
  struct Site {
    const char* where;
    const char* check;
  };
  const Site sites[] = {
      {"voice_queue_ports.slot_free = [this]",
       "!AttachmentBusy()"}, // a voice message waits for the slot, never refused
      {"void ChatController::RefreshShowCapture() {", "AttachmentBusy() || (chat_.attachment_failed"},
      {"void ChatController::BeginAttachmentPreparing(", "if (AttachmentBusy() || !preparing_.Begin("},
      {"void ChatController::SyncComposerInputState() {", "chat_.attachment_in_transcript, AttachmentBusy(),"},
      {"void ChatController::SyncComposerInputState() {", "const bool voice_off = AttachmentBusy() &&"},
      {"void ChatController::OnAttachFile(", "chat_.composer_input_disabled || AttachmentBusy() ||"},
      {"void ChatController::OnAttachCapture(", "chat_.composer_input_disabled || AttachmentBusy() ||"},
      {"void ChatController::RefreshPendingAttachmentChip(", "!messaging_ready_ || AttachmentBusy()) {"},
      {"void ChatController::OnRetryAttachmentSend(", "if (AttachmentBusy() || chat_.voice_recording) {"},
      {"\"home_attach_menu\"", "chat_.composer_input_disabled || AttachmentBusy() ||"},
  };
  for (const Site& site : sites) {
    SCOPED_TRACE(site.where);
    const size_t at = source.find(site.where);
    ASSERT_NE(at, std::string::npos);
    const size_t check = source.find(site.check, at);
    ASSERT_NE(check, std::string::npos);
    EXPECT_LT(check - at, 2500u) << "in that entry";
  }
  // 12: the refusal point lets the queue's own next item through a gap the queue holds, nobody else.
  EXPECT_NE(Upload().find("const bool busy = from_picture_queue ? chat_.attachment_uploading || "
                          "chat_.attachment_preparing : AttachmentBusy();"),
            std::string::npos);
  // No new ad-hoc copy: the pair appears only in the predicate, the refusal point and RefreshAttachmentHere (5: the
  // chip / locked input belong to a real send, never to a gap — the text stays typeable while a queue runs).
  EXPECT_EQ(Count(source, "chat_.attachment_uploading || chat_.attachment_preparing"), 3u);
  EXPECT_NE(Body(source, "void ChatController::RefreshAttachmentHere() {")
                .find("chat_.attachment_uploading || chat_.attachment_preparing || chat_.attachment_failed"),
            std::string::npos);
  EXPECT_EQ(Count(source, "!chat_.attachment_uploading && !chat_.attachment_preparing"), 0u);
}

// --- Without a queue a send goes as it did ---

TEST(AttachmentUploadExit, PictureQueueASingleSendWithoutAQueueIsUnchanged) {
  const std::string source = Controller();
  const std::string body = Upload();
  // Only the queue passes from_picture_queue = true; every other caller keeps the defaults.
  EXPECT_EQ(Count(source, "/*from_picture_queue=*/true"), 1u);
  EXPECT_NE(Body(source, "PictureSendPorts ChatController::MakePictureSendPorts(").find("/*from_picture_queue=*/true"),
            std::string::npos);
  // Busy: without a queue AttachmentBusy() is uploading || preparing (above); the refusal still says why.
  EXPECT_NE(body.find("UserFeedback::Fail(Tr(\"chat.attach.previous_still_sending\"));"), std::string::npos);
  // The picked file is released as before when it is not a queue's (both places).
  EXPECT_EQ(Count(body, "if (!from_picture_queue) {\n"), 2u);
  // The failed bubble keeps its own staged copy and gives way to the next send (the loop's skip is the queue's only).
  EXPECT_NE(body.find("step.next.record_id = new_record;"), std::string::npos);
  EXPECT_NE(body.find("pending_uploads_.Apply(item.row_id, OnOtherAttachmentStarted)"), std::string::npos);
  EXPECT_NE(body.find("/*redraw=*/!sent);"), std::string::npos);
  // A failure still says why, the pace refusal included; only a queue's (picture or voice) pace refusal is quiet.
  EXPECT_NE(body.find("const bool queue_retries =\n                  (from_picture_queue || voice_file) && ended == "
                      "AttachmentSendOutcome::RateLimited;"),
            std::string::npos);
  EXPECT_NE(body.find("if (!cancelled && !queue_retries) {"), std::string::npos);
  // A video keeps its pick check (a queue's item has none).
  EXPECT_NE(body.find("const bool pick_current = from_picture_queue || picks_.IsCurrent(pick_serial);"),
            std::string::npos);
}

TEST(AttachmentUploadExit, PictureQueueAnUnusedWrapperDoesNothing) {
  // StartAttachmentUpload without on_finished (every caller but the queue): the wrapper is empty.
  const AttachmentSendOnce none{std::function<void(AttachmentSendOutcome)>()};
  none.Fire(AttachmentSendOutcome::Sent);
  EXPECT_FALSE(none.Fired());
}

// --- The queue's wiring ---

TEST(AttachmentUploadExit, PictureQueueIsWiredToTheExitsAndTheOriginChat) {
  const std::string source = Controller();
  const std::string ports = Body(source, "PictureSendPorts ChatController::MakePictureSendPorts(");
  ASSERT_FALSE(ports.empty());
  EXPECT_NE(ports.find("AppRuntime::PostUI([this, id, path, current, alive, outcome]()"), std::string::npos)
      << "an item's end is taken in a later UI task";
  EXPECT_NE(ports.find("picture_queue_->ThreadId()"), std::string::npos) << "sent to the chat it was picked in";
  EXPECT_NE(ports.find("facade_->RouteMessage(thread_id, text, std::nullopt)"), std::string::npos);
  EXPECT_EQ(ports.find("SendUserText"), std::string::npos) << "never the chat open now";
  EXPECT_EQ(ports.find("SendWithMentions"), std::string::npos) << "groups only";
  EXPECT_NE(ports.find("PutTextBack(thread_id, text, DraftPutBack::Append);"), std::string::npos);
  EXPECT_NE(ports.find("release_pick_(path); // its queue is gone"), std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::Update(").find("picture_queue_->Locked();"), std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::OnProfileDataReset(").find("picture_queue_->SignedOut();"),
            std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::Shutdown(").find("picture_queue_->SignedOut();"), std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::CloseChatWith(").find("picture_queue_->ChatDeleted();"),
            std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::SettlePictureQueue(").find("picture_queue_->Settled()"),
            std::string::npos);
}

} // namespace
} // namespace pbr
