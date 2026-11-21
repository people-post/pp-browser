// A voice message being sent, drawn as the finished voice bubble with the upload disc over its play button
// (DEV/PLAN-voice-hold-to-record.md follow-up): the same bubble size in every state, the picture's disc events by row id.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/PendingUploadRows.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/voice/VoiceFile.h"

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

PendingUpload VoiceUpload(const PendingUploadState state, const int percent) {
  PendingUpload upload = StartPendingUpload(1, "t", std::nullopt, PendingUploadKind::Voice);
  upload.state = state;
  upload.percent = percent;
  upload.title = VoiceFileName(4200);
  return upload;
}

TEST(TranscriptPendingVoiceTest, UploadingIsTheVoiceBubbleWithTheCancelDiscAndThePercent) {
  const std::string rml = PendingVoiceRml(VoiceUpload(PendingUploadState::Uploading, 40), TranscriptFacts{}, 0);
  EXPECT_NE(rml.find("chat-voice-card chat-voice-card--out"), std::string::npos) << "our voice bubble";
  EXPECT_NE(rml.find("0:05"), std::string::npos) << "its length, from the file's name (4.2 s rounds up)";
  EXPECT_NE(rml.find("cancel_attachment_send('pending-upload-1')"), std::string::npos);
  EXPECT_EQ(rml.find("retry_attachment_send"), std::string::npos);
  EXPECT_EQ(rml.find("discard_attachment_send"), std::string::npos);
  EXPECT_NE(rml.find(">40%</span>"), std::string::npos);
  EXPECT_NE(rml.find("id=\"" + PendingRingElementId("pending-upload-1") + "\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"" + PendingPercentElementId("pending-upload-1") + "\""), std::string::npos);
  EXPECT_NE(rml.find("class=\"chat-voice-wave\""), std::string::npos);
}

TEST(TranscriptPendingVoiceTest, FailedShowsTheRetryDiscAndTheCornerCrossNotWords) {
  const std::string rml = PendingVoiceRml(VoiceUpload(PendingUploadState::Failed, 40), TranscriptFacts{}, 0);
  EXPECT_NE(rml.find("retry_attachment_send('pending-upload-1')"), std::string::npos);
  EXPECT_NE(rml.find("discard_attachment_send('pending-upload-1')"), std::string::npos);
  EXPECT_EQ(rml.find("cancel_attachment_send"), std::string::npos);
  EXPECT_NE(rml.find("icons/v2/retry.svg"), std::string::npos);
  EXPECT_EQ(rml.find("chat-file-percent"), std::string::npos) << "no percent after a failure (as for a picture)";
}

TEST(TranscriptPendingVoiceTest, TheRowsRmlComesFromTheQueueByKind) {
  PendingUploadRows rows([](const std::string&) {});
  PendingUpload item = VoiceUpload(PendingUploadState::Uploading, 10);
  rows.Apply({item, {}});
  TranscriptFacts facts;
  (void)rows.Render({}, facts, std::nullopt, "t", /*at_tip=*/true);
  ASSERT_TRUE(rows.Row());
  const std::string rml = rows.Row()->content_rml.c_str();
  EXPECT_NE(rml.find("chat-voice-card"), std::string::npos);
  EXPECT_EQ(rml.find("send-mark"), std::string::npos);
}

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

std::string AssetPath(const char* relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

constexpr int kWidth = 390;
constexpr int kHeight = 844;

class PendingVoiceLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("pending-voice-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      for (const char* name : {"cancel_attachment_send", "retry_attachment_send", "discard_attachment_send"}) {
        ctor.BindEventCallback(name, [this, name](ui::DataModelHandle, ui::Event&, const ui::VariantList& args) {
          events.emplace_back(name, args.empty() ? std::string() : std::string(args[0].Get<ui::String>().c_str()));
        });
      }
      for (const char* name : {"play_voice", "cancel_picking"}) {
        ctor.BindEventCallback(name, [](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {});
      }
    }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("pending-voice-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  void Show(std::vector<MessageDisplayRow> rows, const char* colors = "themes/colors-light.rcss") {
    state.messages = std::move(rows);
    state.use_messages_layout = true;
    state.has_turns = true;
    if (document) {
      document->Close();
      document = nullptr;
    }
    const LocalizationService& i18n = LocalizationService::Instance();
    const std::string rml =
        "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) + ReadAll(AssetPath(colors)) +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div style=\"position: absolute; top: 0; left: 0; width: " +
        std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
        "dp;\"><div class=\"shell-layer shell-layer-base shell-layer-compact\">"
        "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
        "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" +
        i18n.LocalizeText(ReadAll(AssetPath("views/chat.rml"))) + "</div></div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  ui::Element* First(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  }
  static ui::Vector2f Size(ui::Element* element) { return element->GetBox().GetSize(ui::BoxArea::Border); }
  static ui::Vector2f At(ui::Element* element) { return element->GetAbsoluteOffset(ui::BoxArea::Border); }
  static MessageDisplayRow Row(const std::string& id, const std::string& content) {
    MessageDisplayRow row;
    row.message_id = id.c_str();
    row.row_class = "message-row-user";
    row.content_rml = content.c_str();
    row.run_end = true;
    return row;
  }
  /** The sent voice bubble as the transcript draws it for message `id`. */
  static std::string SentBubble(const std::string& id) {
    ChatAttachmentFields fields;
    fields.mime = kVoiceMime;
    fields.filename = VoiceFileName(4200);
    fields.byte_length = 9000;
    fields.url = "https://example.invalid/blob";
    fields.content_hash.assign(32, 1);
    fields.content_key.assign(32, 2);
    fields.blob_nonce.assign(24, 3);
    TranscriptRow sent;
    sent.message.id = id;
    sent.message.sender_contact_id = kLocalSelfContactId;
    sent.message.content_type = ChatContentType::Attachment;
    sent.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    sent.message.timestamp = 1;
    TranscriptFacts facts;
    facts.voice_supported = true;
    facts.voice_max_seconds = 120;
    facts.attachment = [](const ThreadMessage&) {
      TranscriptAttachmentView view;
      view.state = AttachmentFetchWorkflow::DownloadState::Ready;
      return view;
    };
    const auto rows = RenderTranscript({sent}, facts);
    return rows.empty() ? std::string() : std::string(rows.front().content_rml.c_str());
  }

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
  std::vector<std::pair<std::string, std::string>> events;
};

TEST_F(PendingVoiceLayoutTest, SendingFailedAndSentAreTheSameBubbleSizeInBothThemes) {
  for (const char* colors : {"themes/colors-light.rcss", "themes/colors-dark.rcss"}) {
    SCOPED_TRACE(colors);
    Show({Row("m1", SentBubble("m1"))}, colors);
    ui::Element* sent = First("chat-voice-card");
    ASSERT_NE(sent, nullptr);
    const ui::Vector2f sent_size = Size(sent);
    const ui::Vector2f sent_at = At(sent);
    ASSERT_GT(sent_size.x, 100.f);
    for (const PendingUploadState state_now :
         {PendingUploadState::Uploading, PendingUploadState::Failed, PendingUploadState::Cancelling}) {
      SCOPED_TRACE(static_cast<int>(state_now));
      Show({Row("pending-upload-1", PendingVoiceRml(VoiceUpload(state_now, 40), TranscriptFacts{}, 0))}, colors);
      ui::Element* card = First("chat-voice-card");
      ASSERT_NE(card, nullptr);
      EXPECT_NEAR(Size(card).x, sent_size.x, 0.5f) << "no jump in width between sending and sent";
      EXPECT_NEAR(Size(card).y, sent_size.y, 0.5f) << "no jump in height";
      EXPECT_NEAR(At(card).x, sent_at.x, 0.5f) << "nor in position";
      EXPECT_NEAR(At(card).y, sent_at.y, 0.5f);
    }
  }
}

TEST_F(PendingVoiceLayoutTest, TheDiscSitsOverThePlayButtonAndTheCrossInTheCardsCorner) {
  Show({Row("pending-upload-1", PendingVoiceRml(VoiceUpload(PendingUploadState::Failed, 40), TranscriptFacts{}, 0))});
  ui::Element* disc = First("chat-pending-disc");
  ui::Element* play = First("chat-voice-play");
  ui::Element* card = First("chat-voice-card");
  ui::Element* discard = First("chat-pending-discard");
  ASSERT_NE(disc, nullptr);
  ASSERT_NE(play, nullptr);
  ASSERT_NE(discard, nullptr);
  EXPECT_NEAR(Size(disc).x, 36.f, 0.5f);
  const ui::Vector2f disc_centre = At(disc) + Size(disc) * 0.5f;
  const ui::Vector2f play_centre = At(play) + Size(play) * 0.5f;
  EXPECT_NEAR(disc_centre.x, play_centre.x, 1.f);
  EXPECT_NEAR(disc_centre.y, play_centre.y, 1.f);
  EXPECT_NEAR(Size(discard).x, 44.f, 0.5f);
  EXPECT_LT(std::fabs(At(discard).x + 44.f - (At(card).x + Size(card).x)), 20.f) << "at the card's right end";
}

TEST_F(PendingVoiceLayoutTest, TheDiscCancelsAndRetriesAndTheCrossDiscardsTheRightRow) {
  Show({Row("pending-upload-2", PendingVoiceRml(
                                    [] {
                                      PendingUpload u = VoiceUpload(PendingUploadState::Uploading, 40);
                                      u.row_id = "pending-upload-2";
                                      return u;
                                    }(),
                                    TranscriptFacts{}, 0))});
  First("chat-pending-disc")->Click();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].first, "cancel_attachment_send");
  EXPECT_EQ(events[0].second, "pending-upload-2");
  events.clear();
  Show({Row("pending-upload-3", PendingVoiceRml(
                                    [] {
                                      PendingUpload u = VoiceUpload(PendingUploadState::Failed, 40);
                                      u.row_id = "pending-upload-3";
                                      return u;
                                    }(),
                                    TranscriptFacts{}, 0))});
  First("chat-pending-disc")->Click();
  First("chat-pending-discard")->Click();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0], (std::pair<std::string, std::string>{"retry_attachment_send", "pending-upload-3"}));
  EXPECT_EQ(events[1], (std::pair<std::string, std::string>{"discard_attachment_send", "pending-upload-3"}));
}

TEST_F(PendingVoiceLayoutTest, ProgressIsWrittenIntoTheMountedVoiceBubbleWithoutDrawingItAgain) {
  Show({Row("pending-upload-1", PendingVoiceRml(VoiceUpload(PendingUploadState::Uploading, 0), TranscriptFacts{}, 0))});
  ui::Element* percent = document->GetElementById(PendingPercentElementId("pending-upload-1"));
  ASSERT_NE(percent, nullptr);
  ASSERT_TRUE(PendingUploadRows::DrawProgress(document, "pending-upload-1", 45));
  EXPECT_EQ(std::string(percent->GetInnerRML().c_str()), "45%");
}

} // namespace
} // namespace pbr
