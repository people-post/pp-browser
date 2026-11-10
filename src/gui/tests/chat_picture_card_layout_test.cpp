// A picture card in the phone chat (Kenneth 2026-11-09: the photo sat narrower than its white card): the real chat.rml
// on the real stylesheets at 390dp, rows carrying the card as TranscriptRml draws it; the picture's texture has the
// file's own pixel size, as the engine reads it.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& size, const ui::String&) override {
    size = {intrinsic_w, intrinsic_h};
    return 2;
  }
  int intrinsic_w = 100;
  int intrinsic_h = 100;
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

constexpr int kWidth = 390;
constexpr int kHeight = 844;

class ChatPictureCardLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    // Real glyph widths, so a long title or time takes its real room.
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("chat-picture-card-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(
        context, "chat", [this](ui::DataModelConstructor& ctor) { BindChatScreenState(ctor, state); }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("chat-picture-card-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  /** The phone chat overlay (as ShellLayoutRml lays it out) with `rows` in its transcript. */
  void Show(std::vector<MessageDisplayRow> rows) {
    state.messages = std::move(rows);
    state.use_messages_layout = true;
    state.has_turns = true;
    const LocalizationService& i18n = LocalizationService::Instance();
    const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
                            ReadAll(AssetPath("themes/colors-light.rcss")) +
                            "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
                            "dp;\">"
                            "<div class=\"shell-layer shell-layer-base shell-layer-compact\">"
                            "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
                            "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" +
                            i18n.LocalizeText(ReadAll(AssetPath("views/chat.rml"))) +
                            "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  struct Edges {
    float left, top, right, bottom;
  };
  static Edges BorderEdges(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  /** The `index`-th element with class `cls` (document order). */
  ui::Element* Nth(const char* cls, const size_t index) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return index < found.size() ? found[index] : nullptr;
  }
  static MessageDisplayRow Row(const std::string& id, const char* row_class, const std::string& content) {
    MessageDisplayRow row;
    row.message_id = id.c_str();
    row.row_class = row_class;
    row.content_rml = content.c_str();
    row.run_end = true;
    return row;
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

TranscriptRow PictureRow(const std::string& id) {
  ChatAttachmentFields fields;
  fields.mime = "image/jpeg";
  fields.filename = "photo.jpg";
  fields.byte_length = 50000;
  fields.url = "https://example.invalid/blob";
  fields.content_hash.assign(32, 1);
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

struct Case {
  const char* name;
  int w, h; // the file's pixels; the header read gives 0 x 0 when `known` is false
  bool known;
};

TEST_F(ChatPictureCardLayoutTest, ThePictureFillsItsCardWhateverTheFileSizeKnowledge) {
  const Case cases[] = {{"phone screenshot", 1170, 2532, true},
                        {"landscape photo", 4032, 3024, true},
                        {"header unreadable", 1170, 2532, false},
                        {"header unreadable, landscape", 4032, 3024, false}};
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    renderer.intrinsic_w = c.w;
    renderer.intrinsic_h = c.h;
    TranscriptFacts facts;
    facts.attachment = [&](const ThreadMessage&) {
      TranscriptAttachmentView view;
      view.state = AttachmentFetchWorkflow::DownloadState::Ready;
      view.local_path =
          std::string("/tmp/photo-") + c.name + ".jpg"; // one texture per case (the engine caches by source)
      view.inline_image_ok = true;
      view.image_width = c.known ? c.w : 0;
      view.image_height = c.known ? c.h : 0;
      return view;
    };
    const auto rows = RenderTranscript({PictureRow("p")}, facts);
    ASSERT_EQ(rows.size(), 1u);
    std::vector<MessageDisplayRow> shown;
    shown.push_back(rows[0]);
    Show(std::move(shown));
    ui::Element* card = Nth("chat-attachment-card--image", 0);
    ui::Element* clip = Nth("chat-attachment-clip", 0);
    ui::Element* image = Nth("chat-attachment-image", 0);
    ASSERT_NE(card, nullptr);
    ASSERT_NE(clip, nullptr);
    ASSERT_NE(image, nullptr);
    const Edges k = BorderEdges(card);
    const Edges l = BorderEdges(clip);
    const Edges i = BorderEdges(image);
    printf("[%s] card %.1f x %.1f  clip %.1f x %.1f  img %.1f x %.1f at (%.1f,%.1f) clip at (%.1f,%.1f)\n", c.name,
           k.right - k.left, k.bottom - k.top, l.right - l.left, l.bottom - l.top, i.right - i.left, i.bottom - i.top,
           i.left, i.top, l.left, l.top);
    EXPECT_NEAR(i.right - i.left, l.right - l.left, 0.5f) << "the picture is as wide as its clip";
    EXPECT_NEAR(i.left, l.left, 0.5f);
    EXPECT_NEAR(k.right - k.left, (l.right - l.left) + 6.f, 0.5f) << "card = clip + 3dp frame a side";
    EXPECT_NEAR(l.right - l.left, 292.f, 1.f) << "a big picture is the box wide";
    EXPECT_LE(l.bottom - l.top, 360.5f);
    EXPECT_GE(i.bottom - i.top, l.bottom - l.top - 0.5f) << "the picture covers its clip";
    EXPECT_LE(i.top, l.top + 0.5f);
    EXPECT_GE(i.bottom, l.bottom - 0.5f);
  }
}

} // namespace
} // namespace pbr
