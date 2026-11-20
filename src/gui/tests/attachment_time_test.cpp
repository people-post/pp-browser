// The send time of attachment bubbles (2026-11-20): a picture, a video, a file, a sound file and a voice message show
// their time exactly when a text bubble in the same position shows its own (the clock time when it is known), with the
// same route icon rule (none while queued), on a phone, in both themes. The real chat.rml on the real stylesheets.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <ui/style/ComputedValues.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& size, const ui::String&) override {
    size = {600, 450};
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

std::string AssetPath(const std::string& relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

constexpr int kWidth = 390;
constexpr int kHeight = 2400;
constexpr int64_t kSentAt = 1763640000000; // a time of day well inside a day, whatever the zone

bool Shown(ui::Element* element) {
  for (ui::Element* at = element; at != nullptr; at = at->GetParentNode()) {
    if (at->GetComputedValues().display() == ui::Style::Display::None) {
      return false;
    }
  }
  return element->GetBox().GetSize(ui::BoxArea::Border).x > 0.f;
}

struct Case {
  const char* theme;
  bool ours;
};

class AttachmentTimeTest : public testing::TestWithParam<Case> {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    context = ui::CreateContext("attachment-time-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      for (const char* name : {"open_attachment", "download_attachment", "retry_attachment", "play_audio_file",
                               "audio_seek_press", "play_voice", "open_sender", "jump_to_quote"}) {
        ctor.BindEventCallback(name, [](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {});
      }
    }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("attachment-time-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  ThreadMessage Message(const std::string& id, const ChatContentType type, const int64_t at,
                        const std::string& payload = {}) const {
    ThreadMessage message;
    message.id = id;
    message.sender_contact_id = GetParam().ours ? kLocalSelfContactId : "contact-bea";
    message.content_type = type;
    message.payload_json = payload;
    message.timestamp = at;
    message.transport = MessageTransport::Direct;
    message.text = type == ChatContentType::Text ? "hello" : "";
    return message;
  }
  static std::string Attachment(const std::string& name, const std::string& mime, const uint64_t bytes) {
    ChatAttachmentFields fields;
    fields.filename = name;
    fields.mime = mime;
    fields.byte_length = bytes;
    fields.url = "https://example.invalid/blob";
    fields.content_hash.assign(32, 1);
    fields.content_key.assign(32, 2);
    fields.blob_nonce.assign(24, 3);
    return ChatPayloadCodec::AttachmentFieldsToJson(fields);
  }

  /** text, picture, video, file, sound file, voice message: sent at `at` (0: unknown), as `mark` says. */
  void ShowAll(const int64_t at, const SendMark mark) {
    std::vector<TranscriptRow> rows;
    for (const auto& message :
         {Message("t", ChatContentType::Text, at),
          Message("p", ChatContentType::Attachment, at, Attachment("a.jpg", "image/jpeg", 50000)),
          Message("v", ChatContentType::Attachment, at, Attachment("c.mp4", "video/mp4", 900000)),
          Message("f", ChatContentType::Attachment, at, Attachment("d.pdf", "application/pdf", 90000)),
          Message("m", ChatContentType::Attachment, at, Attachment("e.mp3", "audio/mpeg", 3000000)),
          Message("s", ChatContentType::Attachment, at, Attachment("voice-3000.ogg", "audio/ogg", 20000))}) {
      TranscriptRow row;
      row.message = message;
      rows.push_back(row);
    }
    TranscriptFacts facts;
    facts.voice_supported = true;
    facts.audio_files_play = true;
    facts.send_mark = [mark](const ThreadMessage&) { return mark; };
    facts.attachment = [](const ThreadMessage& message) {
      TranscriptAttachmentView view;
      view.state = AttachmentFetchWorkflow::DownloadState::Ready;
      view.inline_image_ok = true;
      view.image_width = 4032;
      view.image_height = 3024;
      if (message.id == "p") {
        view.local_path = "/x/a.jpg";
      } else if (message.id == "v") {
        view.poster_path = "/x/poster.jpg";
      } else {
        view.on_demand = true;
      }
      return view;
    };
    state.messages = RenderTranscript(rows, facts);
    state.use_messages_layout = true;
    state.has_turns = true;
    const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
                            ReadAll(AssetPath(std::string("themes/colors-") + GetParam().theme + ".rcss")) +
                            "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
                            "dp;\"><div class=\"shell-layer shell-layer-base shell-layer-compact\">"
                            "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
                            "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" +
                            LocalizationService::Instance().LocalizeText(ReadAll(AssetPath("views/chat.rml"))) +
                            "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->ActivateTheme(GetParam().theme, true);
    context->Update();
    context->Update();
  }

  std::vector<ui::Element*> All(const char* selector) {
    ui::ElementList found;
    document->QuerySelectorAll(found, selector);
    return {found.begin(), found.end()};
  }
  /** The shown element of `selector` with a clock time in it ("18:32"), or null. */
  ui::Element* TimeIn(const char* selector) {
    for (ui::Element* element : All(selector)) {
      if (Shown(element) && std::string(element->GetInnerRML().c_str()).find(':') != std::string::npos) {
        return element;
      }
    }
    return nullptr;
  }
  static bool HasRouteIcon(ui::Element* time) {
    return std::string(time->GetInnerRML().c_str()).find("route-icon") != std::string::npos;
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

// The selectors of each kind's time, text bubble first.
const char* const kTimeSelectors[] = {
    ".bubble .bubble-time",                               // text
    ".chat-attachment-card--image .chat-attachment-time", // picture
    ".chat-attachment-video-frame .chat-attachment-time", // video
    ".chat-file-card .chat-attachment-clock",             // file
    ".chat-audio-card .chat-attachment-clock",            // sound file
    ".chat-voice-clock",                                  // voice message
};

TEST_P(AttachmentTimeTest, EveryKindShowsItsTimeWhenTheTextBubbleDoes) {
  ShowAll(kSentAt, SendMark::None);
  for (const char* selector : kTimeSelectors) {
    SCOPED_TRACE(selector);
    ui::Element* time = TimeIn(selector);
    ASSERT_NE(time, nullptr) << "a bubble with a known time shows it";
    EXPECT_TRUE(HasRouteIcon(time)) << "the route icon, as a text bubble's";
  }
}

TEST_P(AttachmentTimeTest, NoKindShowsATimeWhenItIsUnknown) {
  ShowAll(0, SendMark::None);
  EXPECT_EQ(TimeIn(kTimeSelectors[0]), nullptr) << "the text bubble shows none";
  for (const char* selector : kTimeSelectors) {
    SCOPED_TRACE(selector);
    EXPECT_EQ(TimeIn(selector), nullptr);
  }
}

TEST_P(AttachmentTimeTest, WhileQueuedOrFailedTheTimeStaysAndTheRouteIconGoesLikeInTextBubbles) {
  if (!GetParam().ours) {
    GTEST_SKIP() << "a send mark is only ever on our own messages";
  }
  for (const SendMark mark : {SendMark::Queued, SendMark::Failed}) {
    ShowAll(kSentAt, mark);
    for (const char* selector : kTimeSelectors) {
      SCOPED_TRACE(selector);
      ui::Element* time = TimeIn(selector);
      ASSERT_NE(time, nullptr) << "the time is shown, as on a text bubble";
      EXPECT_FALSE(HasRouteIcon(time)) << "it has not travelled yet";
    }
    context->UnloadDocument(document);
    document = nullptr;
  }
}

TEST_P(AttachmentTimeTest, ThePicturesAndVideosTimeIsAPillInsideTheirBottomRightCorner) {
  ShowAll(kSentAt, SendMark::None);
  for (const char* parent : {".chat-attachment-card--image", ".chat-attachment-video-frame"}) {
    SCOPED_TRACE(parent);
    ui::Element* time = TimeIn((std::string(parent) + " .chat-attachment-time").c_str());
    ASSERT_NE(time, nullptr);
    ui::Element* frame = All(parent).front();
    const ui::Vector2f frame_at = frame->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f frame_size = frame->GetBox().GetSize(ui::BoxArea::Border);
    const ui::Vector2f at = time->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = time->GetBox().GetSize(ui::BoxArea::Border);
    EXPECT_LE(at.x + size.x, frame_at.x + frame_size.x + 0.5f);
    EXPECT_LE(at.y + size.y, frame_at.y + frame_size.y + 0.5f);
    EXPECT_GT(at.x, frame_at.x + frame_size.x / 2.f) << "in the right half";
    EXPECT_GT(at.y, frame_at.y + frame_size.y / 2.f) << "in the lower half";
    EXPECT_NE(time->GetComputedValues().background_color().alpha, 0) << "a pill, so it reads on any picture";
  }
}

TEST_P(AttachmentTimeTest, TheFileAndSoundCardsTimeIsAtTheirBottomRightInsideThem) {
  ShowAll(kSentAt, SendMark::None);
  for (const char* parent : {".chat-file-card", ".chat-audio-card"}) {
    SCOPED_TRACE(parent);
    ui::Element* time = TimeIn((std::string(parent) + " .chat-attachment-clock").c_str());
    ASSERT_NE(time, nullptr);
    ui::Element* card = All(parent).front();
    const ui::Vector2f card_at = card->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f card_size = card->GetBox().GetSize(ui::BoxArea::Border);
    const ui::Vector2f at = time->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = time->GetBox().GetSize(ui::BoxArea::Border);
    EXPECT_LE(at.x + size.x, card_at.x + card_size.x + 0.5f);
    EXPECT_LE(at.y + size.y, card_at.y + card_size.y + 0.5f);
    EXPECT_GT(at.y, card_at.y + card_size.y / 2.f) << "at the bottom";
  }
}

INSTANTIATE_TEST_SUITE_P(ThemesAndSides, AttachmentTimeTest,
                         testing::Values(Case{"light", true}, Case{"light", false}, Case{"dark", true},
                                         Case{"dark", false}));

} // namespace
} // namespace pbr
