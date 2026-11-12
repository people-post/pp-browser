// B203 (DEV/PLAN-chat-picture-previews.md): which chat picture textures are released, and under which name. The
// count is per file (the same picture sent twice is one file on two rows); and the name is the one the UI library files
// an <img src> under — on the real engine (a null renderer): with that name ui::ReleaseTexture finds the loaded texture,
// with the raw absolute path (what the step-0 probe passed: 0 hits in 10) it does not.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatPictureTextures.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/VoiceBubble.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace pbr {
namespace {

MessageDisplayRow Row(const std::string& id, const std::string& rml) {
  MessageDisplayRow row;
  row.message_id = id.c_str();
  row.content_rml = rml.c_str();
  row.row_class = "message-row-peer";
  row.run_end = true;
  return row;
}

MessageDisplayRow Picture(const std::string& id, const std::string& src) {
  TranscriptAttachmentView view;
  view.local_path = src;
  view.inline_image_ok = true;
  view.image_width = 640;
  view.image_height = 480;
  return Row(id, "<div class=\"chat-card chat-attachment-card chat-attachment-card--image\">" +
                     PictureBubbleClipRml(id, "ab", view) + "</div>");
}

std::vector<std::string> Sorted(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  return v;
}

TEST(ChatPictureTexturesTest, PictureSourcesAreUnescapedInRowOrder) {
  const std::vector<MessageDisplayRow> rows = {
      Row("1", "<div><img class=\"chat-attachment-image\" src=\"/a b/1.jpg\" width=\"10\"/></div>"),
      Row("2", "<p>no picture</p>"),
      Row("3", "<img class=\"chat-attachment-image\" src=\"/x&amp;y.jpg\"/>"),
      Row("4", "<img class=\"chat-attachment-video-poster\" src=\"/poster.jpg\" width=\"216\"/>"),
      Row("5",
          "<div class=\"chat-audio-cover\"><img class=\"chat-audio-cover-image\" src=\"/cover.poster.jpg\"/></div>"),
      Row("6", "<img class=\"avatar-photo chat-voice-avatar-photo\" src=\"/icons/peer.png\"/>"),
  };
  // Pictures, video posters and sound covers come from the view cache; a sender's photo does not.
  EXPECT_EQ(PictureSources(rows),
            (std::vector<std::string>{"/a b/1.jpg", "/x&y.jpg", "/poster.jpg", "/cover.poster.jpg"}));
}

/** A video card with its poster and a music card with its cover, as the transcript draws them. */
std::vector<MessageDisplayRow> PosterAndCoverRows() {
  TranscriptFacts facts;
  facts.audio_files_play = true;
  facts.attachment = [](const ThreadMessage& message) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/v/" + message.id + ".bin";
    view.poster_path = "/v/" + message.id + ".poster.jpg";
    view.image_width = 640;
    view.image_height = 360;
    return view;
  };
  std::vector<TranscriptRow> rows;
  for (const auto& [id, mime, name] : {std::tuple<std::string, std::string, std::string>{"vid", "video/mp4", "a.mp4"},
                                       {"song", "audio/mpeg", "a.mp3"}}) {
    ChatAttachmentFields fields;
    fields.mime = mime;
    fields.filename = name;
    fields.byte_length = 900'000;
    fields.url = "https://example.invalid/blob";
    fields.content_hash.assign(32, static_cast<uint8_t>(id.size()));
    fields.content_key.assign(32, 2);
    fields.blob_nonce.assign(24, 3);
    TranscriptRow row;
    row.message.id = id;
    row.message.thread_id = "t";
    row.message.sender_contact_id = "contact-1";
    row.message.content_type = ChatContentType::Attachment;
    row.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    row.message.timestamp = 1;
    rows.push_back(std::move(row));
  }
  return RenderTranscript(rows, facts);
}

TEST(ChatPictureTexturesTest, PostersAndCoversAreCountedAndGoUnderTheLockLikePictures) {
  std::vector<MessageDisplayRow> rows = PosterAndCoverRows();
  rows.push_back(Picture("pic", "/v/pic.poster.jpg"));
  ChatPictureTextures textures;
  EXPECT_TRUE(textures.Mount(rows).empty());
  EXPECT_EQ(textures.Count("/v/vid.poster.jpg"), 1u);
  EXPECT_EQ(textures.Count("/v/song.poster.jpg"), 1u);
  EXPECT_EQ(textures.Count("/v/pic.poster.jpg"), 1u);

  const ChatPictureTextures::Locked locked = textures.DropAllForLock(rows);
  EXPECT_TRUE(locked.rows_changed);
  EXPECT_EQ(Sorted(locked.gone),
            (std::vector<std::string>{"/v/pic.poster.jpg", "/v/song.poster.jpg", "/v/vid.poster.jpg"}));
  EXPECT_TRUE(PictureSources(rows).empty()) << "nothing on the page names a wiped file";
  const std::string video = rows[0].content_rml.c_str();
  const std::string song = rows[1].content_rml.c_str();
  EXPECT_EQ(video.find("<img"), std::string::npos) << video;
  EXPECT_NE(video.find("chat-attachment-video-frame"), std::string::npos) << "the frame stays, empty: " << video;
  EXPECT_NE(video.find("chat-attachment-video-play"), std::string::npos) << video;
  EXPECT_EQ(song.find("<img"), std::string::npos) << song;
  EXPECT_NE(song.find(kAudioCoverNoteRml), std::string::npos) << "the cover it has without a picture: " << song;
  EXPECT_EQ(textures.Count("/v/vid.poster.jpg"), 0u);
  EXPECT_FALSE(textures.DropAllForLock(rows).rows_changed) << "nothing left to take";
}

// A row trimmed off the page or a chat left: its poster and cover are released like a picture.
TEST(ChatPictureTexturesTest, APosterOrCoverOffThePageIsReleased) {
  const std::vector<MessageDisplayRow> rows = PosterAndCoverRows();
  ChatPictureTextures textures;
  (void)textures.Mount(rows);
  EXPECT_EQ(textures.Mount({rows[1]}), (std::vector<std::string>{"/v/vid.poster.jpg"}));
  EXPECT_EQ(textures.UnmountAll(), (std::vector<std::string>{"/v/song.poster.jpg"}));
}

TEST(ChatPictureTexturesTest, RowsTrimmedOffThePageReleaseTheirPictures) {
  ChatPictureTextures textures;
  EXPECT_TRUE(textures.Mount({Picture("1", "/v/a.jpg"), Picture("2", "/v/b.jpg"), Picture("3", "/v/c.jpg")}).empty());
  // The window keeps the newest two (TrimDomWindow drops the oldest).
  EXPECT_EQ(textures.Mount({Picture("2", "/v/b.jpg"), Picture("3", "/v/c.jpg")}),
            (std::vector<std::string>{"/v/a.jpg"}));
  EXPECT_EQ(textures.Count("/v/a.jpg"), 0u);
  EXPECT_EQ(textures.Count("/v/b.jpg"), 1u);
}

TEST(ChatPictureTexturesTest, AnotherChatReleasesEveryPictureOfTheFirst) {
  ChatPictureTextures textures;
  (void)textures.Mount({Picture("1", "/t1/a.jpg"), Picture("2", "/t1/b.jpg")});
  EXPECT_EQ(Sorted(textures.Mount({Picture("9", "/t2/z.jpg")})), (std::vector<std::string>{"/t1/a.jpg", "/t1/b.jpg"}));
}

TEST(ChatPictureTexturesTest, ADeletedMessageReleasesItsPicture) {
  ChatPictureTextures textures;
  (void)textures.Mount({Picture("1", "/v/a.jpg"), Picture("2", "/v/b.jpg")});
  // "Delete for me": the row stays as "This message was deleted", without its picture.
  EXPECT_EQ(textures.Mount({Row("1", "<p class=\"text muted\">deleted</p>"), Picture("2", "/v/b.jpg")}),
            (std::vector<std::string>{"/v/a.jpg"}));
}

TEST(ChatPictureTexturesTest, TheLockAndLeavingTheChatReleaseEverything) {
  ChatPictureTextures textures;
  (void)textures.Mount({Picture("1", "/v/a.jpg"), Picture("2", "/v/b.jpg"), Picture("3", "/v/a.jpg")});
  EXPECT_EQ(Sorted(textures.UnmountAll()), (std::vector<std::string>{"/v/a.jpg", "/v/b.jpg"}));
  EXPECT_EQ(textures.Count("/v/a.jpg"), 0u);
  EXPECT_TRUE(textures.UnmountAll().empty());
}

// The lock turns the pictures into placeholders before their textures go: nothing on the page points at them.
TEST(ChatPictureTexturesTest, AfterTheLocksPlaceholdersNoPictureIsLeftOnThePage) {
  std::vector<MessageDisplayRow> rows = {Picture("1", "/v/a.jpg"), Picture("2", "/v/b.jpg")};
  for (MessageDisplayRow& row : rows) {
    std::string rml = row.content_rml.c_str();
    ASSERT_TRUE(DropPictureImages(rml));
    row.content_rml = rml.c_str();
  }
  EXPECT_TRUE(PictureSources(rows).empty());
}

TEST(ChatPictureTexturesTest, OneFileOnTwoRowsStaysUntilNeitherShowsIt) {
  ChatPictureTextures textures;
  (void)textures.Mount({Picture("1", "/v/same.jpg"), Picture("2", "/v/same.jpg")});
  EXPECT_EQ(textures.Count("/v/same.jpg"), 2u);
  EXPECT_TRUE(textures.Mount({Picture("2", "/v/same.jpg")}).empty()) << "one row left the page: the other shows it";
  EXPECT_TRUE(textures.Mount({Row("1", "<p>deleted</p>"), Picture("2", "/v/same.jpg")}).empty())
      << "one deleted: the other shows it";
  EXPECT_EQ(textures.Mount({}), (std::vector<std::string>{"/v/same.jpg"}));
}

// --- The library's name for a texture --------------------------------------------------------------------------------

class CountingRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& size, const ui::String& source) override {
    size = {640, 480};
    loaded.push_back(source);
    return 7;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle handle) override { released += handle == 7 ? 1 : 0; }
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
  std::vector<std::string> loaded;
  int released = 0;
};

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string AssetPath(const char* relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

TEST(ChatPictureTexturesTest, TheKeyDropsTheLeadingSlashOfAnAbsolutePath) {
  EXPECT_EQ(TextureKeyForImageSrc("/var/mobile/a b/x.poster.jpg"), "var/mobile/a b/x.poster.jpg");
  EXPECT_EQ(TextureKeyForImageSrc("C:\\Users\\k\\x.jpg"), "C:\\Users\\k\\x.jpg");
}

TEST(ChatPictureTexturesTest, ReleaseFindsTheLoadedTextureByTheLibrarysNameAndNotByTheRawPath) {
  CountingRender renderer;
  ui::SetRenderInterface(&renderer);
  ASSERT_TRUE(ui::Initialise());
  ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
  ui::Context* context = ui::CreateContext("texture-key-test", {390, 844});
  ASSERT_NE(context, nullptr);
  ChatScreenState state;
  ASSERT_TRUE(DataModelHost::Instance().Register(
      context, "chat", [&state](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state); }));
  const std::string src = "/private/var/mobile/Containers/Data/App Support/threads/t/blobs_view/ab.poster.jpg";
  state.messages = {Picture("1", src)};
  state.use_messages_layout = true;
  state.has_turns = true;
  const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
                          "</style></head><body style=\"font-family: Inter;\"><div class=\"shell-chat-overlay\">" +
                          LocalizationService::Instance().LocalizeText(ReadAll(AssetPath("views/chat.rml"))) +
                          "</div></body></rml>";
  ui::ElementDocument* document = context->LoadDocumentFromMemory(rml, "/app/assets/views/shell.rml");
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Render();
  ASSERT_EQ(renderer.loaded.size(), 1u) << "the picture was loaded as it was drawn";
  EXPECT_EQ(renderer.loaded[0], TextureKeyForImageSrc(src)) << "the name it was loaded under";
  const ui::StringList sources = ui::GetTextureSourceList();
  EXPECT_TRUE(std::find(sources.begin(), sources.end(), TextureKeyForImageSrc(src)) != sources.end());

  EXPECT_FALSE(ui::ReleaseTexture(src)) << "the raw absolute path is not its name (the step-0 probe's miss)";
  EXPECT_TRUE(ui::ReleaseTexture(TextureKeyForImageSrc(src)));
  EXPECT_EQ(renderer.released, 1);
  EXPECT_FALSE(ui::ReleaseTexture(TextureKeyForImageSrc(src))) << "released once: nothing loaded under it now";

  DataModelHost::Instance().Clear();
  ui::RemoveContext("texture-key-test");
  ui::Shutdown();
  ui::SetRenderInterface(nullptr);
}

// The lock on the real engine: the rows lose their pictures, posters and covers first, then the textures go, and
// nothing loads them again. Released while its <img> is still on the page, a texture is loaded again at the next
// drawing (from a file the lock has wiped: the library would remember that failure) — why the order matters.
TEST(ChatPictureTexturesTest, OnTheEngineTheLockTakesThemOffThePageBeforeTheTexturesGoAndNothingLoadsThemAgain) {
  CountingRender renderer;
  ui::SetRenderInterface(&renderer);
  ASSERT_TRUE(ui::Initialise());
  ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
  ui::Context* context = ui::CreateContext("texture-lock-test", {390, 844});
  ASSERT_NE(context, nullptr);
  ChatScreenState state;
  ASSERT_TRUE(DataModelHost::Instance().Register(
      context, "chat", [&state](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state); }));
  state.messages = PosterAndCoverRows();
  state.messages.push_back(Picture("pic", "/v/pic.poster.jpg"));
  state.use_messages_layout = true;
  state.has_turns = true;
  const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
                          "</style></head><body style=\"font-family: Inter;\"><div class=\"shell-chat-overlay\">" +
                          LocalizationService::Instance().LocalizeText(ReadAll(AssetPath("views/chat.rml"))) +
                          "</div></body></rml>";
  ui::ElementDocument* document = context->LoadDocumentFromMemory(rml, "/app/assets/views/shell.rml");
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  context->Render();
  const auto loaded_of = [&renderer](const std::string& src) {
    return std::count(renderer.loaded.begin(), renderer.loaded.end(), TextureKeyForImageSrc(src));
  };
  for (const char* src : {"/v/vid.poster.jpg", "/v/song.poster.jpg", "/v/pic.poster.jpg"}) {
    ASSERT_EQ(loaded_of(src), 1) << src << " drawn";
  }

  // Released while the <img> is still there: loaded again.
  ASSERT_TRUE(ui::ReleaseTexture(TextureKeyForImageSrc("/v/pic.poster.jpg")));
  context->Update();
  context->Render();
  EXPECT_EQ(loaded_of("/v/pic.poster.jpg"), 2);

  // The lock: off the page, the page drawn, then released (ChatController::Update, then ReleaseQueuedTextures).
  ChatPictureTextures textures;
  (void)textures.Mount(state.messages);
  ChatPictureTextures::Locked locked = textures.DropAllForLock(state.messages);
  ASSERT_TRUE(locked.rows_changed);
  DirtyChatTurns();
  context->Update();
  context->Render();
  ASSERT_EQ(locked.gone.size(), 3u);
  for (const std::string& src : locked.gone) {
    EXPECT_TRUE(ui::ReleaseTexture(TextureKeyForImageSrc(src))) << src;
  }
  const size_t loads = renderer.loaded.size();
  context->Update();
  context->Render();
  EXPECT_EQ(renderer.loaded.size(), loads) << "nothing on the page asks for them again";
  for (const char* cls : {"chat-attachment-image", "chat-attachment-video-poster", "chat-audio-cover-image"}) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    EXPECT_TRUE(found.empty()) << cls;
  }

  DataModelHost::Instance().Clear();
  ui::RemoveContext("texture-lock-test");
  ui::Shutdown();
  ui::SetRenderInterface(nullptr);
}

} // namespace
} // namespace pbr
