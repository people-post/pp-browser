// The picture being sent, drawn as our picture bubble while it uploads (DEV/PLAN-picture-upload-bubble.md, step 5):
// its RML (events, fixed element ids, no send mark), and the real chat.rml on the real stylesheets at 390dp — the
// bubble the size of the sent picture, the 44dp corner target, and the progress written into the mounted bubble
// without drawing the row again.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/PendingUploadRows.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/delivery/ChatPayloadCodec.h"

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

size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

PendingUpload Upload(const PendingUploadState state, const int percent, const std::string& thumb, const int w = 4032,
                     const int h = 3024) {
  PendingUpload upload = StartPendingUpload(1, "t", PicturePixels{w, h});
  upload.state = state;
  upload.percent = percent;
  upload.thumb_path = thumb;
  return upload;
}

TEST(TranscriptPendingPictureTest, UploadingCancelsShowsThePercentAndCarriesTheFixedIds) {
  const std::string rml = PendingPictureRml(Upload(PendingUploadState::Uploading, 67, "/p/thumb.jpg"));
  EXPECT_EQ(Count(rml, "data-event-click=\"cancel_attachment_send('pending-upload-1')\""), 1u);
  EXPECT_EQ(rml.find("retry_attachment_send"), std::string::npos);
  EXPECT_EQ(rml.find("discard_attachment_send"), std::string::npos);
  EXPECT_NE(rml.find(">67%</p>"), std::string::npos);
  EXPECT_NE(rml.find("id=\"" + PendingRingElementId("pending-upload-1") + "\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"" + PendingPercentElementId("pending-upload-1") + "\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"" + PendingThumbElementId("pending-upload-1") + "\""), std::string::npos);
  EXPECT_NE(rml.find("src=\"/p/thumb.jpg\""), std::string::npos);
  if (kPendingProgressRing) {
    EXPECT_NE(rml.find("decorator: " + PendingRingDecorator(67)), std::string::npos);
  }
}

TEST(TranscriptPendingPictureTest, CancellingOnlySpinsWithNothingToTap) {
  const std::string rml = PendingPictureRml(Upload(PendingUploadState::Cancelling, 40, "/p/thumb.jpg"));
  EXPECT_EQ(rml.find("data-event-click"), std::string::npos);
  EXPECT_NE(rml.find("chat-pending-ring--spin"), std::string::npos);
}

TEST(TranscriptPendingPictureTest, FailedRetriesAndOffersTheCornerDiscard) {
  const std::string rml = PendingPictureRml(Upload(PendingUploadState::Failed, 40, "/p/thumb.jpg"));
  EXPECT_EQ(Count(rml, "data-event-click=\"retry_attachment_send('pending-upload-1')\""), 1u);
  EXPECT_EQ(Count(rml, "data-event-click=\"discard_attachment_send('pending-upload-1')\""), 1u);
  EXPECT_EQ(rml.find("cancel_attachment_send"), std::string::npos);
  EXPECT_NE(rml.find("../icons/v2/retry.svg"), std::string::npos);
  EXPECT_EQ(rml.find(PendingPercentElementId("pending-upload-1")), std::string::npos) << "no percent after a failure";
}

TEST(TranscriptPendingPictureTest, ThePlaceholderHasTheSizeOfTheThumbnailBubble) {
  const std::string with = PendingPictureRml(Upload(PendingUploadState::Uploading, 0, "/p/thumb.jpg"));
  const std::string without = PendingPictureRml(Upload(PendingUploadState::Uploading, 0, ""));
  EXPECT_EQ(without.find("<img"), std::string::npos);
  const PictureBubbleSize fit = FitPictureInBubble(4032, 3024);
  const std::string size =
      "style=\"width:" + std::to_string(fit.width_dp) + "dp;height:" + std::to_string(fit.height_dp) + "dp\"";
  EXPECT_NE(with.find(size), std::string::npos) << "sized as FitPictureInBubble";
  EXPECT_NE(without.find(size), std::string::npos) << "the placeholder keeps the size";
}

TEST(TranscriptPendingPictureTest, ATallPictureIsSizedAndCroppedAsTheSentOne) {
  const std::string rml = PendingPictureRml(Upload(PendingUploadState::Uploading, 0, "/p/t.jpg", 1170, 2532));
  const PictureBubbleSize fit = FitPictureInBubble(1170, 2532);
  ASSERT_GT(fit.image_offset_dp, 0);
  EXPECT_NE(rml.find("height=\"" + std::to_string(fit.image_height_dp) + "\""), std::string::npos);
  EXPECT_NE(rml.find("margin-top:-" + std::to_string(fit.image_offset_dp) + "dp"), std::string::npos);
}

TEST(TranscriptPendingPictureTest, TheRingIsEmptyAtZeroWholeAtHundredAndCutAtThePercent) {
  EXPECT_EQ(PendingRingDecorator(0), "none");
  EXPECT_EQ(PendingRingDecorator(100), "conic-gradient(white 0%, white 100%)");
  EXPECT_EQ(PendingRingDecorator(25), "conic-gradient(white 0%, white 25%, transparent 25%)");
  EXPECT_EQ(PendingRingDecorator(-3), "none");
  EXPECT_EQ(PendingPercentText(140), "100%");
}

TEST(TranscriptPendingPictureTest, ThePendingRowCarriesNoSendMark) {
  PendingUploadRows rows([](const std::string&) {});
  rows.Apply({StartPendingUpload(1, "t", PicturePixels{800, 600}), {}});
  TranscriptFacts facts;
  int asked = 0;
  facts.send_mark = [&asked](const ThreadMessage&) {
    ++asked;
    return SendMark::Queued; // every row asked would get the clock
  };
  (void)rows.Render({}, facts, std::nullopt, "t", /*at_tip=*/true);
  ASSERT_TRUE(rows.Row());
  EXPECT_EQ(rows.Row()->send_mark, 0);
  EXPECT_EQ(std::string(rows.Row()->content_rml.c_str()).find("send-mark"), std::string::npos);
}

// --- Mounted: the real chat.rml and stylesheets on a 390dp phone. ---

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

class PendingPictureLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("pending-picture-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      for (const char* name : {"cancel_attachment_send", "retry_attachment_send", "discard_attachment_send"}) {
        ctor.BindEventCallback(name, [](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {});
      }
    }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("pending-picture-test");
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
  ui::Element* First(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    return found.empty() ? nullptr : found.front();
  }
  static ui::Vector2f Size(ui::Element* element) { return element->GetBox().GetSize(ui::BoxArea::Border); }
  static MessageDisplayRow Row(const std::string& id, const std::string& content) {
    MessageDisplayRow row;
    row.message_id = id.c_str();
    row.row_class = "message-row-user";
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

TEST_F(PendingPictureLayoutTest, TheBubbleHasTheSentPicturesSizeWithOrWithoutItsThumbnail) {
  // The sent picture's card as TranscriptRml draws it, for the same 4032 x 3024 photo.
  ChatAttachmentFields fields;
  fields.mime = "image/jpeg";
  fields.filename = "photo.jpg";
  fields.byte_length = 50000;
  fields.url = "https://example.invalid/blob";
  fields.content_hash.assign(32, 1);
  fields.content_key.assign(32, 2);
  fields.blob_nonce.assign(24, 3);
  TranscriptRow sent;
  sent.message.id = "m1";
  sent.message.sender_contact_id = kLocalSelfContactId;
  sent.message.content_type = ChatContentType::Attachment;
  sent.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  sent.message.timestamp = 1;
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/x/sent.jpg";
    view.inline_image_ok = true;
    view.image_width = 4032;
    view.image_height = 3024;
    return view;
  };
  Show(RenderTranscript({sent}, facts));
  ui::Element* sent_card = First("chat-attachment-card--image");
  ASSERT_NE(sent_card, nullptr);
  const ui::Vector2f sent_size = Size(sent_card);

  for (const char* thumb : {"/x/thumb.jpg", ""}) {
    SCOPED_TRACE(thumb);
    Show({Row("pending-upload-1", PendingPictureRml(Upload(PendingUploadState::Uploading, 30, thumb)))});
    ui::Element* card = First("chat-pending-picture");
    ASSERT_NE(card, nullptr);
    EXPECT_NEAR(Size(card).x, sent_size.x, 0.5f);
    EXPECT_NEAR(Size(card).y, sent_size.y, 0.5f);
    ui::Element* disc = First("chat-pending-disc");
    ASSERT_NE(disc, nullptr);
    EXPECT_NEAR(Size(disc).x, 48.f, 0.5f);
    EXPECT_NEAR(Size(disc).y, 48.f, 0.5f);
  }
}

TEST_F(PendingPictureLayoutTest, TheCornerDiscardIsAFortyFourDpTargetInTheTopRightCorner) {
  Show({Row("pending-upload-1", PendingPictureRml(Upload(PendingUploadState::Failed, 30, "/x/thumb.jpg")))});
  ui::Element* discard = First("chat-pending-discard");
  ui::Element* clip = First("chat-pending-picture-clip");
  ASSERT_NE(discard, nullptr);
  ASSERT_NE(clip, nullptr);
  EXPECT_NEAR(Size(discard).x, 44.f, 0.5f);
  EXPECT_NEAR(Size(discard).y, 44.f, 0.5f);
  const ui::Vector2f at = discard->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f clip_at = clip->GetAbsoluteOffset(ui::BoxArea::Border);
  EXPECT_NEAR(at.y, clip_at.y, 0.5f);
  EXPECT_NEAR(at.x + 44.f, clip_at.x + Size(clip).x, 0.5f);
}

TEST_F(PendingPictureLayoutTest, ProgressIsWrittenIntoTheMountedBubbleWithoutDrawingTheRowAgain) {
  Show({Row("pending-upload-1", PendingPictureRml(Upload(PendingUploadState::Uploading, 0, "/x/thumb.jpg")))});
  ui::Element* thumb = document->GetElementById(PendingThumbElementId("pending-upload-1"));
  ui::Element* percent = document->GetElementById(PendingPercentElementId("pending-upload-1"));
  ASSERT_NE(thumb, nullptr);
  ASSERT_NE(percent, nullptr);
  ASSERT_TRUE(PendingUploadRows::DrawProgress(document, "pending-upload-1", 45));
  DataModelHost::Instance().Dirty("chat", "messages"); // something else redraws the transcript meanwhile
  context->Update();
  // The same elements (the row was not parsed again, so the picture does not reload), the new values in them.
  EXPECT_EQ(document->GetElementById(PendingThumbElementId("pending-upload-1")), thumb);
  EXPECT_EQ(document->GetElementById(PendingPercentElementId("pending-upload-1")), percent);
  EXPECT_EQ(std::string(percent->GetInnerRML().c_str()), "45%");
  if (kPendingProgressRing) {
    ui::Element* ring = document->GetElementById(PendingRingElementId("pending-upload-1"));
    ASSERT_NE(ring, nullptr);
    EXPECT_NE(std::string(ring->GetProperty("decorator")->ToString().c_str()).find("conic-gradient"),
              std::string::npos);
  }
  EXPECT_FALSE(PendingUploadRows::DrawProgress(nullptr, "pending-upload-1", 50));
}

TEST_F(PendingPictureLayoutTest, LockingTakesTheThumbnailOutOfTheMountedBubble) {
  Show({Row("pending-upload-1", PendingPictureRml(Upload(PendingUploadState::Uploading, 10, "/x/thumb.jpg")))});
  ui::Element* clip = First("chat-pending-picture-clip");
  ASSERT_NE(clip, nullptr);
  const ui::Vector2f before = Size(clip);
  PendingUploadRows::DropThumbnail(document, "pending-upload-1");
  context->Update();
  EXPECT_EQ(document->GetElementById(PendingThumbElementId("pending-upload-1")), nullptr);
  EXPECT_NEAR(Size(First("chat-pending-picture-clip")).y, before.y, 0.5f) << "the bubble keeps its size";
}

} // namespace
} // namespace pbr
