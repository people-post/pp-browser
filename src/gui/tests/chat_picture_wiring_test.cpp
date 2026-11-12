// B203 (DEV/PLAN-chat-picture-previews.md): the chat picture steps tested elsewhere (ChatPictureTextures,
// ApplyReadyPreviews, PhotoPreviewRml, the transcript's taps) only work if ChatController calls them where it must.
// ChatController cannot be built in a unit test (a started messaging hub, the shell), so its wiring is read from the
// source, as OpenLinkSourceTest does: deleting one of these calls fails here.
#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace pbr {
namespace {

/** The file with "\r\n" folded to "\n": a Windows checkout has CRLF line ends (core.autocrlf). */
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
std::optional<std::string> FunctionBody(const std::string& source, const std::string& signature) {
  const size_t at = source.find("\n" + signature);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  const size_t close = source.find("\n}\n", at);
  if (close == std::string::npos) {
    return std::nullopt;
  }
  return source.substr(at + 1, close + 2 - (at + 1));
}

std::string Body(const std::string& file, const std::string& signature) {
  const std::string source = ReadFile(std::string(PP_SOURCE_DIR) + "/" + file);
  EXPECT_FALSE(source.empty()) << file << " not found under PP_SOURCE_DIR";
  const auto body = FunctionBody(source, signature);
  EXPECT_TRUE(body.has_value()) << signature << " not found in " << file;
  return body.value_or(std::string());
}

constexpr const char* kController = "src/gui/chat/ChatController.cpp";

// The lock: the rows lose their pictures, posters and covers (and are drawn again) before their textures are queued
// for release; the release itself runs a frame later (ReleaseQueuedTextures).
TEST(ChatPictureWiringTest, TheLockDropsThePicturesThenQueuesTheirRelease) {
  const std::string update = Body(kController, "void ChatController::Update(");
  const size_t locked = update.find("if (messaging_ready_ && !MessagingReady())");
  const size_t drop = update.find("picture_textures_.DropAllForLock(chat_.messages)");
  const size_t dirty = update.find("DirtyChatTurns()", drop);
  const size_t release = update.find("QueueTextureRelease(std::move(locked.gone))");
  ASSERT_NE(locked, std::string::npos);
  ASSERT_NE(drop, std::string::npos) << "the lock no longer takes the pictures off the page";
  ASSERT_NE(release, std::string::npos) << "the lock no longer releases the textures";
  EXPECT_LT(locked, drop);
  EXPECT_LT(drop, dirty);
  EXPECT_LT(dirty, release) << "drawn without the pictures before the release is queued";
  EXPECT_NE(update.find("ReleaseQueuedTextures()"), std::string::npos);
  const std::string release_body = Body(kController, "void ChatController::ReleaseQueuedTextures(");
  EXPECT_NE(release_body.find("ui::ReleaseTexture(TextureKeyForImageSrc(src))"), std::string::npos);
  EXPECT_NE(release_body.find("textures_release_ready_ = std::exchange(textures_release_queued_, {})"),
            std::string::npos)
      << "queued this frame, released the next one (after the page dropped them)";
}

// A preview finished after the chat was left: not drawn into the hidden rows, not counted (the textures went on
// leaving); a redraw already scheduled finds nothing.
TEST(ChatPictureWiringTest, APreviewForAChatNotShownIsNotDrawnOrCounted) {
  const std::string done = Body(kController, "void ChatController::OnChatPreviewDone(");
  const size_t guard = done.find("!ChatShowing()");
  ASSERT_NE(guard, std::string::npos);
  EXPECT_LT(guard, done.find("preview_batcher_.Add(")) << "checked before it is queued for a redraw";
  const std::string redraw = Body(kController, "void ChatController::RedrawPreviews(");
  const size_t redraw_guard = redraw.find("!ChatShowing()");
  ASSERT_NE(redraw_guard, std::string::npos);
  EXPECT_LT(redraw_guard, redraw.find("ApplyReadyPreviews(")) << "checked before a row changes";
  EXPECT_LT(redraw_guard, redraw.find("SyncPictureTextures()")) << "checked before anything is counted";
  const std::string poll = Body(kController, "void ChatController::PollChatShowing(");
  EXPECT_NE(poll.find("preview_batcher_.Clear()"), std::string::npos);
  EXPECT_NE(poll.find("picture_textures_.UnmountAll()"), std::string::npos) << "leaving still releases";
}

// A tap on a picture (its preview or its placeholder): the spinner, then the open of the full picture found from the
// message; the bubble's preview path never reaches the open.
TEST(ChatPictureWiringTest, ATapOpensTheFullPictureFoundFromTheMessage) {
  const std::string open_picture = Body(kController, "void ChatController::OpenPicture(");
  EXPECT_NE(open_picture.find("PhotoPreviewRml()"), std::string::npos);
  EXPECT_NE(open_picture.find("OpenAttachment(message_id)"), std::string::npos);
  EXPECT_EQ(open_picture.find("poster"), std::string::npos);
  EXPECT_EQ(open_picture.find("local_path"), std::string::npos);

  const std::string open = Body(kController, "void ChatController::OpenAttachment(");
  const size_t path = open.find("facade_->AttachmentOpenPathForMessage(thread_id, message_id, &made_view)");
  const size_t file = open.find("PlatformOpenFile(path)");
  ASSERT_NE(path, std::string::npos);
  ASSERT_NE(file, std::string::npos) << "the open file is no longer called";
  EXPECT_NE(open.find("[path = *path,"), std::string::npos) << "what is opened is the path looked up for the message";

  // That path is the message's own view (made from its stored bytes when missing), never its preview.
  const char* facade = "capabilities/messaging/src/feature/conversations/ConversationsFacade.cpp";
  const std::string local =
      Body(facade, "std::optional<std::string> ConversationsFacade::AttachmentLocalPathForMessage(");
  EXPECT_NE(local.find("AttachmentLocalPath(hub_.ProfileDataDir(), thread_id, fields->content_hash"),
            std::string::npos);
  EXPECT_NE(local.find("EnsureLocalViewPath(thread_id, fields->content_hash"), std::string::npos);
  EXPECT_EQ(local.find("AttachmentPosterPath"), std::string::npos) << "never the preview";
  const std::string named =
      Body(facade, "std::optional<std::string> ConversationsFacade::AttachmentOpenPathForMessage(");
  EXPECT_NE(named.find("AttachmentLocalPathForMessage(thread_id, message_id, made_view)"), std::string::npos);
  EXPECT_EQ(named.find("AttachmentPosterPath"), std::string::npos);
}

} // namespace
} // namespace pbr
