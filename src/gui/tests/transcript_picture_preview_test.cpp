// B203 (DEV/PLAN-chat-picture-previews.md): a chat picture's bubble shows its preview, or a placeholder of the same size
// until the preview is made; a picture whose preview cannot be made is a file card. A preview made later replaces
// only that picture's clip in its row, at most every 250 ms: on the real chat.rml, the row elements are the same ones
// after the redraw and the other rows' contents are untouched.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/PreviewRedrawBatcher.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/TranscriptAssembly.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

TranscriptRow PictureRow(const std::string& id, const uint8_t hash_byte = 1) {
  ChatAttachmentFields fields;
  fields.mime = "image/jpeg";
  fields.filename = "photo.jpg";
  fields.byte_length = 900'000;
  fields.url = "https://example.invalid/blob";
  fields.content_hash.assign(32, hash_byte);
  fields.content_key.assign(32, 2);
  fields.blob_nonce.assign(24, 3);
  TranscriptRow row;
  row.message.id = id;
  row.message.thread_id = "t";
  row.message.sender_contact_id = "contact-1";
  row.message.content_type = ChatContentType::Attachment;
  row.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  row.message.timestamp = 1;
  return row;
}

TranscriptAttachmentView ReadyView() {
  TranscriptAttachmentView view;
  view.state = AttachmentFetchWorkflow::DownloadState::Ready;
  view.inline_image_ok = true;
  view.image_width = 3024;
  view.image_height = 4032;
  return view;
}

TranscriptAttachmentView PlaceholderView() {
  TranscriptAttachmentView view = ReadyView();
  view.picture_placeholder = true;
  return view;
}

TranscriptAttachmentView PreviewView(const std::string& preview = "/views/aa.poster.jpg") {
  TranscriptAttachmentView view = ReadyView();
  view.local_path = preview;
  return view;
}

std::string RenderOne(const TranscriptAttachmentView& view, const std::string& id = "m1") {
  TranscriptFacts facts;
  facts.attachment = [view](const ThreadMessage&) { return view; };
  const auto rows = RenderTranscript({PictureRow(id)}, facts);
  EXPECT_EQ(rows.size(), 1u);
  return rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str());
}

/** The clip's inline style (its size). */
std::string ClipStyle(const std::string& rml) {
  const size_t clip = rml.find("<div class=\"chat-attachment-clip");
  const size_t style = rml.find(" style=\"", clip);
  const size_t end = rml.find('"', style + 8);
  return clip == std::string::npos || style == std::string::npos ? std::string() : rml.substr(style, end - style);
}

TEST(TranscriptPicturePreviewTest, ThePlaceholderIsTheSizeOfThePictureThatReplacesIt) {
  const std::string placeholder = RenderOne(PlaceholderView());
  const std::string preview = RenderOne(PreviewView());
  EXPECT_NE(placeholder.find("chat-picture-placeholder"), std::string::npos) << placeholder;
  EXPECT_EQ(placeholder.find("<img"), std::string::npos) << "nothing to decode yet";
  EXPECT_FALSE(ClipStyle(placeholder).empty());
  EXPECT_EQ(ClipStyle(placeholder), ClipStyle(preview)) << "nothing moves when the preview comes";
}

TEST(TranscriptPicturePreviewTest, ThePreviewIsWhatTheBubbleShowsAndATapNamesOnlyTheMessage) {
  const std::string preview = RenderOne(PreviewView("/views/aa.poster.jpg"));
  EXPECT_NE(preview.find("<img class=\"chat-attachment-image\" src=\"/views/aa.poster.jpg\""), std::string::npos)
      << preview;
  EXPECT_NE(preview.find("open_picture('m1')"), std::string::npos) << "the open path is found from the message";
  EXPECT_EQ(preview.find("poster.jpg')"), std::string::npos) << "the preview's path is not handed to the open";
  const std::string placeholder = RenderOne(PlaceholderView());
  EXPECT_NE(placeholder.find("open_picture('m1')"), std::string::npos) << "the placeholder opens the picture too";
}

TEST(TranscriptPicturePreviewTest, APictureWhosePreviewCannotBeMadeIsAFileCard) {
  TranscriptAttachmentView failed = ReadyView();
  failed.local_path = "/views/aa.jpg"; // its full view: what the card opens
  failed.inline_image_ok = false;
  const std::string rml = RenderOne(failed);
  EXPECT_EQ(rml.find("chat-attachment-clip"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("<img"), std::string::npos);
  EXPECT_NE(rml.find("open_attachment('m1')"), std::string::npos) << rml;
}

TEST(TranscriptPicturePreviewTest, APreviewReplacesOnlyItsOwnClip) {
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage& message) {
    return message.id == "m1" ? PlaceholderView() : PreviewView("/views/bb.poster.jpg");
  };
  auto rows = RenderTranscript({PictureRow("m1", 1), PictureRow("m2", 2)}, facts);
  ASSERT_EQ(rows.size(), 2u);
  const std::string hex1 = AttachmentHashHex(std::vector<uint8_t>(32, 1));
  const std::string hex2 = AttachmentHashHex(std::vector<uint8_t>(32, 2));
  EXPECT_TRUE(RowShowsPicturePlaceholder(rows[0], hex1));
  EXPECT_FALSE(RowShowsPicturePlaceholder(rows[0], hex2));
  EXPECT_FALSE(RowShowsPicturePlaceholder(rows[1], hex2)) << "a picture already shown";

  std::string rml = rows[0].content_rml.c_str();
  const std::string before = rml;
  ASSERT_TRUE(ReplacePictureClip(rml, "m1", PictureBubbleClipRml("m1", hex1, PreviewView("/views/aa.poster.jpg"))));
  EXPECT_NE(rml.find("src=\"/views/aa.poster.jpg\""), std::string::npos);
  EXPECT_EQ(rml.find("chat-picture-placeholder"), std::string::npos);
  // Everything round the clip is as it was.
  const size_t clip_at = before.find("<div class=\"chat-attachment-clip");
  EXPECT_EQ(rml.substr(0, clip_at), before.substr(0, clip_at));
  const size_t old_end = before.find("</div>", clip_at) + 6;
  const size_t new_end = rml.find("</div>", rml.find("<img", clip_at)) + 6;
  EXPECT_EQ(rml.substr(new_end), before.substr(old_end));
  EXPECT_FALSE(ReplacePictureClip(rml, "m9", "<div/>")) << "no clip of that message";
}

TEST(TranscriptPicturePreviewTest, TheLockTurnsEveryChatPictureIntoItsPlaceholder) {
  std::string rml = RenderOne(PreviewView("/views/aa.poster.jpg"));
  ASSERT_TRUE(DropPictureImages(rml));
  EXPECT_EQ(rml.find("<img"), std::string::npos) << rml;
  EXPECT_NE(rml.find("chat-attachment-clip chat-picture-placeholder\""), std::string::npos) << rml;
  EXPECT_FALSE(DropPictureImages(rml)) << "nothing left to take";
  std::string text = "<p class=\"text\">hello</p>";
  EXPECT_FALSE(DropPictureImages(text));
}

// A video or sound file whose cover is not there (AttachmentView gives no path for a missing file or under the lock)
// draws no <img>: the video's frame without a poster, the music note.
TEST(TranscriptPicturePreviewTest, NoCoverPathNoImg) {
  for (const auto& [mime, name] : {std::pair<std::string, std::string>{"video/mp4", "a.mp4"}, {"audio/mpeg", "a.mp3"}}) {
    TranscriptRow row = PictureRow("m1");
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(row.message.payload_json);
    ASSERT_TRUE(fields);
    fields->mime = mime;
    fields->filename = name;
    row.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(*fields);
    TranscriptFacts facts;
    facts.audio_files_play = true;
    facts.attachment = [](const ThreadMessage&) {
      TranscriptAttachmentView view = ReadyView();
      view.local_path = "/views/aa.bin";
      return view; // no poster_path
    };
    const auto rows = RenderTranscript({row}, facts);
    ASSERT_EQ(rows.size(), 1u);
    const std::string rml = rows[0].content_rml.c_str();
    EXPECT_EQ(rml.find("<img"), std::string::npos) << mime << ": " << rml;
  }
}

// A placeholder (no preview file, or locked) draws no <img> either.
TEST(TranscriptPicturePreviewTest, APlaceholderNamesNoFile) {
  TranscriptAttachmentView view = PlaceholderView();
  const std::string rml = RenderOne(view);
  EXPECT_EQ(rml.find("<img"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("poster"), std::string::npos) << rml;
}

TEST(TranscriptPicturePreviewTest, RedrawsAreAtMostOneEvery250Ms) {
  using Clock = PreviewRedrawBatcher::Clock;
  PreviewRedrawBatcher batcher;
  const Clock::time_point t0 = Clock::now();
  const auto first = batcher.Add("a", t0);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->count(), 0) << "the first one at once";
  EXPECT_FALSE(batcher.Add("b", t0 + std::chrono::milliseconds(10)).has_value()) << "one redraw is coming";
  EXPECT_EQ(batcher.Take(t0 + std::chrono::milliseconds(20)), (std::vector<std::string>{"a", "b"}));
  const auto second = batcher.Add("c", t0 + std::chrono::milliseconds(30));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->count(), 240) << "250 ms after the last redraw";
  EXPECT_FALSE(batcher.Add("d", t0 + std::chrono::milliseconds(100)).has_value());
  EXPECT_FALSE(batcher.Add("c", t0 + std::chrono::milliseconds(120)).has_value());
  EXPECT_EQ(batcher.Take(t0 + std::chrono::milliseconds(270)), (std::vector<std::string>{"c", "d"}));
  const auto later = batcher.Add("e", t0 + std::chrono::seconds(2));
  ASSERT_TRUE(later.has_value());
  EXPECT_EQ(later->count(), 0);
  batcher.Clear(); // another chat opened before it ran
  EXPECT_TRUE(batcher.Take(t0 + std::chrono::seconds(2)).empty());
}

// --- On the real chat.rml ------------------------------------------------------------------------------------------

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& size, const ui::String&) override {
    size = {480, 640};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

std::string AssetPath(const char* relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

class TranscriptPicturePreviewDomTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("picture-preview-test", {390, 844});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context, "chat", [this](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state); }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("picture-preview-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  void Show(std::vector<MessageDisplayRow> rows) {
    state.messages = std::move(rows);
    state.use_messages_layout = true;
    state.has_turns = true;
    const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
                            ReadAll(AssetPath("themes/colors-light.rcss")) +
                            "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: 390dp; height: 844dp;\">"
                            "<div class=\"shell-layer shell-layer-base shell-layer-compact\">"
                            "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
                            "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" +
                            LocalizationService::Instance().LocalizeText(ReadAll(AssetPath("views/chat.rml"))) +
                            "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  ui::Element* Nth(const char* cls, const size_t index) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return index < found.size() ? found[index] : nullptr;
  }
  static ui::Vector2f Size(ui::Element* element) { return element->GetBox().GetSize(ui::BoxArea::Border); }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

TEST_F(TranscriptPicturePreviewDomTest, ThePlaceholderBoxIsLaidOutAsThePictureWillBe) {
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage& message) {
    return message.id == "m1" ? PlaceholderView() : PreviewView("/views/bb.poster.jpg");
  };
  Show(RenderTranscript({PictureRow("m1", 1), PictureRow("m2", 2)}, facts));
  ui::Element* placeholder = Nth("chat-picture-placeholder", 0);
  ui::Element* clip_of_picture = Nth("chat-attachment-clip", 1);
  ASSERT_NE(placeholder, nullptr);
  ASSERT_NE(clip_of_picture, nullptr);
  EXPECT_GT(Size(placeholder).x, 100.f);
  EXPECT_NEAR(Size(placeholder).x, Size(clip_of_picture).x, 0.5f);
  EXPECT_NEAR(Size(placeholder).y, Size(clip_of_picture).y, 0.5f);
}

TEST_F(TranscriptPicturePreviewDomTest, APreviewRedrawKeepsTheRowElementsAndTheOtherRowsContent) {
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage& message) {
    return message.id == "m1" ? PlaceholderView() : PreviewView("/views/bb.poster.jpg");
  };
  Show(RenderTranscript({PictureRow("m1", 1), PictureRow("m2", 2)}, facts));
  ui::ElementList rows_before;
  document->QuerySelectorAll(rows_before, "[message-id]");
  ASSERT_GE(rows_before.size(), 2u);
  ui::Element* other_picture = Nth("chat-attachment-image", 0); // m2's, the only <img> so far
  ASSERT_NE(other_picture, nullptr);
  ASSERT_NE(Nth("chat-picture-placeholder", 0), nullptr);

  const std::string hex1 = AttachmentHashHex(std::vector<uint8_t>(32, 1));
  std::string rml = state.messages[0].content_rml.c_str();
  ASSERT_TRUE(ReplacePictureClip(rml, "m1", PictureBubbleClipRml("m1", hex1, PreviewView("/views/aa.poster.jpg"))));
  state.messages[0].content_rml = rml.c_str();
  DirtyChatTurns();
  context->Update();

  ui::ElementList rows_after;
  document->QuerySelectorAll(rows_after, "[message-id]");
  ASSERT_EQ(rows_after.size(), rows_before.size());
  for (size_t i = 0; i < rows_after.size(); ++i) {
    EXPECT_EQ(rows_after[i], rows_before[i]) << "row " << i << " was not built again";
  }
  EXPECT_EQ(Nth("chat-picture-placeholder", 0), nullptr);
  ui::ElementList images;
  document->GetElementsByClassName(images, "chat-attachment-image");
  ASSERT_EQ(images.size(), 2u);
  EXPECT_TRUE(std::find(images.begin(), images.end(), other_picture) != images.end())
      << "the other row's picture element is the same one: its content was not reset";
}

} // namespace
} // namespace pbr
