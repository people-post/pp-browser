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
#include "lib/messaging/AttachmentCache.h"

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
      for (const char* name :
           {"cancel_attachment_send", "retry_attachment_send", "discard_attachment_send", "cancel_picking"}) {
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

// --- Step 2 of DEV/PLAN-attachment-bubbles-multi-pick.md: file and video bubbles. ---

TranscriptRow SentAttachment(const std::string& id, const std::string& name, const std::string& mime,
                             const uint64_t bytes) {
  ChatAttachmentFields fields;
  fields.mime = mime;
  fields.filename = name;
  fields.byte_length = bytes;
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
  return sent;
}

TranscriptFacts ReadyFacts(const std::string& poster = {}, const int w = 0, const int h = 0) {
  TranscriptFacts facts;
  facts.attachment = [poster, w, h](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/x/file.bin";
    view.poster_path = poster;
    view.image_width = w;
    view.image_height = h;
    return view;
  };
  return facts;
}

PendingUpload FileItem(const PendingUploadState state, const std::string& title, const std::string& size_text,
                       const int percent = 0) {
  PendingUpload item = StartPendingUpload(1, "t", std::nullopt, PendingUploadKind::File);
  item.state = state;
  item.title = title;
  item.size_text = size_text;
  item.percent = percent;
  return item;
}

TEST(TranscriptPendingFileTest, TheTypeTileShowsTheExtensionInCapitalsUpToFourLetters) {
  EXPECT_EQ(FileTypeTileLabel("report.pdf"), "PDF");
  EXPECT_EQ(FileTypeTileLabel("archive.tar.gz"), "GZ");
  EXPECT_EQ(FileTypeTileLabel("slides.Pptx"), "PPTX");
  EXPECT_EQ(FileTypeTileLabel("data.parquet"), "") << "longer than 4: the generic icon";
  EXPECT_EQ(FileTypeTileLabel("README"), "");
  EXPECT_EQ(FileTypeTileLabel(".bashrc"), "");
  EXPECT_EQ(FileTypeTileLabel("a.b-c"), "");
  const std::string generic = PendingFileRml(FileItem(PendingUploadState::Uploading, "README", ""));
  EXPECT_NE(generic.find("../icons/v2/file.svg"), std::string::npos);
  EXPECT_NE(PendingFileRml(FileItem(PendingUploadState::Uploading, "a.zip", "")).find(">ZIP</span>"),
            std::string::npos);
}

TEST(TranscriptPendingFileTest, TheNameIsEscapedAndTheSizeWaitsWithAnEllipsis) {
  const std::string rml = PendingFileRml(FileItem(PendingUploadState::Uploading, "a \"b\" <c> & d.txt", ""));
  EXPECT_NE(rml.find("a &quot;b&quot; &lt;c&gt; &amp; d.txt"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("<c>"), std::string::npos);
  EXPECT_NE(rml.find(">…</p>"), std::string::npos);
  EXPECT_NE(PendingFileRml(FileItem(PendingUploadState::Uploading, "x.txt", "2.4 MB")).find(">2.4 MB</p>"),
            std::string::npos);
}

TEST(TranscriptPendingFileTest, UploadingPutsThePercentInTheDiscOnTheTileAndCancels) {
  const std::string rml = PendingFileRml(FileItem(PendingUploadState::Uploading, "r.pdf", "1 MB", 67));
  const size_t tile = rml.find("class=\"chat-file-tile\"");
  const size_t disc = rml.find("class=\"chat-pending-disc", tile);
  const size_t percent = rml.find("id=\"" + PendingPercentElementId("pending-upload-1") + "\"", disc);
  const size_t text = rml.find("class=\"chat-file-text\"");
  ASSERT_NE(tile, std::string::npos);
  ASSERT_NE(disc, std::string::npos);
  ASSERT_NE(percent, std::string::npos);
  EXPECT_LT(percent, text) << "the percent is inside the disc on the tile";
  EXPECT_NE(rml.find(">67%</span>"), std::string::npos);
  EXPECT_NE(rml.find("cancel_attachment_send('pending-upload-1')"), std::string::npos);
  EXPECT_EQ(rml.find("close.svg"), std::string::npos) << "no cross: the percent is in the middle";
}

TEST(TranscriptPendingFileTest, FailedRetriesAndOffersTheDiscard) {
  const std::string rml = PendingFileRml(FileItem(PendingUploadState::Failed, "r.pdf", "1 MB"));
  EXPECT_NE(rml.find("retry_attachment_send('pending-upload-1')"), std::string::npos);
  EXPECT_NE(rml.find("discard_attachment_send('pending-upload-1')"), std::string::npos);
  EXPECT_EQ(rml.find("cancel_attachment_send"), std::string::npos);
}

TEST(TranscriptPendingFileTest, TheSentFileCardHasTheTileAndOpensFromTheWholeCard) {
  const auto rows = RenderTranscript({SentAttachment("f1", "report.pdf", "application/pdf", 2'400'000)}, ReadyFacts());
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  const size_t card = rml.find("chat-file-card");
  ASSERT_NE(card, std::string::npos) << rml;
  const size_t card_end = rml.find('>', card);
  EXPECT_NE(rml.substr(card, card_end - card).find("data-event-click=\"open_attachment('f1')\""), std::string::npos)
      << "the card itself opens the file";
  EXPECT_NE(rml.find(">PDF</span>"), std::string::npos);
  EXPECT_EQ(rml.find("chat-suggestion"), std::string::npos) << "no separate Open button";
}

TEST(TranscriptPendingVideoTest, NoPlayKeyWhileItUploadsAndTheBareFrameWithoutAPoster) {
  PendingUpload framed = StartPendingUpload(1, "t", PicturePixels{1080, 1920}, PendingUploadKind::Video);
  framed.thumb_path = "/p/poster.jpg";
  const std::string rml = PendingUploadRml(framed);
  EXPECT_EQ(rml.find("chat-attachment-video-play"), std::string::npos);
  EXPECT_NE(rml.find("cancel_attachment_send('pending-upload-1')"), std::string::npos);
  EXPECT_NE(rml.find("src=\"/p/poster.jpg\""), std::string::npos);
  const PictureBubbleSize fit = FitPictureInBubble(1080, 1920);
  EXPECT_NE(rml.find("width:" + std::to_string(fit.width_dp) + "dp;height:" + std::to_string(fit.height_dp) + "dp"),
            std::string::npos)
      << "a portrait video is a portrait frame from the start";
  PendingUpload bare = StartPendingUpload(2, "t", std::nullopt, PendingUploadKind::Video);
  bare.title = "clip.mov";
  EXPECT_NE(PendingUploadRml(bare).find("chat-attachment-video-frame--bare"), std::string::npos);
}

TEST_F(PendingPictureLayoutTest, AFileBubbleAndTheSentCardItTurnsIntoHaveTheSameSize) {
  for (const char* name : {"report.pdf", "a-rather-long-file-name-of-forty-chars.zip", "README"}) {
    SCOPED_TRACE(name);
    Show(RenderTranscript({SentAttachment("f1", name, "application/octet-stream", 2'400'000)}, ReadyFacts()));
    ui::Element* sent = First("chat-file-card");
    ASSERT_NE(sent, nullptr);
    const ui::Vector2f sent_size = Size(sent);
    Show({Row("pending-upload-1",
              PendingFileRml(FileItem(PendingUploadState::Uploading, name, FormatAttachmentByteSize(2'400'000), 30)))});
    ui::Element* pending = First("chat-file-card");
    ASSERT_NE(pending, nullptr);
    EXPECT_NEAR(Size(pending).x, sent_size.x, 0.5f);
    EXPECT_NEAR(Size(pending).y, sent_size.y, 0.5f);
    EXPECT_GE(Size(pending).x, 219.5f);
    EXPECT_LE(Size(pending).x, 292.5f);
  }
}

TEST_F(PendingPictureLayoutTest, TheDiscAndItsPercentSitInsideTheTypeTile) {
  Show({Row("pending-upload-1", PendingFileRml(FileItem(PendingUploadState::Uploading, "r.pdf", "1 MB", 40)))});
  ui::Element* tile = First("chat-file-tile");
  ui::Element* disc = First("chat-pending-disc");
  ui::Element* percent = document->GetElementById(PendingPercentElementId("pending-upload-1"));
  ASSERT_NE(tile, nullptr);
  ASSERT_NE(disc, nullptr);
  ASSERT_NE(percent, nullptr);
  EXPECT_NEAR(Size(tile).x, 72.f, 0.5f);
  const auto inside = [](ui::Element* inner, ui::Element* outer) {
    const ui::Vector2f a = inner->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f b = outer->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f as = Size(inner);
    const ui::Vector2f bs = Size(outer);
    return a.x >= b.x - 0.5f && a.y >= b.y - 0.5f && a.x + as.x <= b.x + bs.x + 0.5f && a.y + as.y <= b.y + bs.y + 0.5f;
  };
  EXPECT_TRUE(inside(disc, tile));
  EXPECT_TRUE(inside(percent, disc));
}

TEST_F(PendingPictureLayoutTest, TheFileDiscardIsAFortyFourDpTarget) {
  Show({Row("pending-upload-1", PendingFileRml(FileItem(PendingUploadState::Failed, "r.pdf", "1 MB")))});
  ui::Element* discard = First("chat-file-discard");
  ASSERT_NE(discard, nullptr);
  EXPECT_NEAR(Size(discard).x, 44.f, 0.5f);
  EXPECT_NEAR(Size(discard).y, 44.f, 0.5f);
}

TEST_F(PendingPictureLayoutTest, AFortyCharacterNameKeepsRoomForTwelveCharactersAtTheNarrowestCard) {
  Show({Row("pending-upload-1", PendingFileRml(FileItem(PendingUploadState::Uploading,
                                                        "abcdefghijabcdefghijabcdefghijabcdefghij", "1 MB")))});
  ui::Element* name = First("chat-file-name");
  ASSERT_NE(name, nullptr);
  // 12 characters of 15dp Inter take about 90dp.
  EXPECT_GE(Size(name).x, 90.f);
  EXPECT_GT(Size(name).y, 15.f);
}

TEST_F(PendingPictureLayoutTest, AVideoBubbleHasTheSentVideosSizeAndKeepsItWhenItsSizeLineArrives) {
  Show(RenderTranscript({SentAttachment("v1", "clip.mov", "video/quicktime", 12'000'000)},
                        ReadyFacts("/x/poster.jpg", 1080, 1920)));
  ui::Element* sent = First("chat-attachment-card");
  ASSERT_NE(sent, nullptr);
  const ui::Vector2f sent_size = Size(sent);
  ui::Element* frame = First("chat-attachment-video-frame");
  ASSERT_NE(frame, nullptr);
  EXPECT_LE(Size(frame).x, sent_size.x) << "the poster stays inside its card (it ran past a 240dp card)";
  PendingUpload video = StartPendingUpload(1, "t", PicturePixels{1080, 1920}, PendingUploadKind::Video);
  video.thumb_path = "/p/poster.jpg";
  Show({Row("pending-upload-1", PendingUploadRml(video))});
  const ui::Vector2f before = Size(First("chat-attachment-card"));
  video = OnUploadDetails(video, "clip.mov", FormatAttachmentByteSize(12'000'000)).next;
  Show({Row("pending-upload-1", PendingUploadRml(video))});
  const ui::Vector2f after = Size(First("chat-attachment-card"));
  EXPECT_NEAR(after.y, before.y, 0.5f) << "the size line arriving does not move it";
  EXPECT_NEAR(after.x, sent_size.x, 0.5f);
  EXPECT_NEAR(after.y, sent_size.y, 0.5f);
  // No poster: the sent card's bare frame.
  Show(RenderTranscript({SentAttachment("v2", "clip.mov", "video/quicktime", 12'000'000)}, ReadyFacts()));
  const ui::Vector2f sent_bare = Size(First("chat-attachment-video-frame--bare"));
  PendingUpload bare = StartPendingUpload(2, "t", std::nullopt, PendingUploadKind::Video);
  bare.title = "clip.mov";
  Show({Row("pending-upload-2", PendingUploadRml(bare))});
  EXPECT_NEAR(Size(First("chat-attachment-video-frame--bare")).x, sent_bare.x, 0.5f);
  EXPECT_NEAR(Size(First("chat-attachment-video-frame--bare")).y, sent_bare.y, 0.5f);
}

// Step 3: the centred "Preparing…" overlay: one line, a second after 5 s, and Cancel.
TEST_F(PendingPictureLayoutTest, ThePreparingOverlayHasItsLinesAndCancel) {
  const auto shown = [this](const char* cls) {
    ui::Element* element = First(cls);
    return element && element->IsVisible(true);
  };
  Show({});
  EXPECT_FALSE(shown("chat-picking-overlay"));
  state.picking_busy = true;
  DataModelHost::Instance().Dirty("chat", "picking_busy");
  context->Update();
  EXPECT_TRUE(shown("chat-picking-overlay"));
  EXPECT_TRUE(shown("chat-picking-spin"));
  EXPECT_FALSE(shown("chat-picking-line--slow"));
  ui::Element* cancel = First("chat-picking-cancel");
  ASSERT_NE(cancel, nullptr);
  EXPECT_NE(std::string(cancel->GetAttribute<ui::String>("data-event-click", "").c_str()).find("cancel_picking"),
            std::string::npos);
  EXPECT_GE(Size(cancel).y, 44.f);
  state.picking_busy_slow = true;
  DataModelHost::Instance().Dirty("chat", "picking_busy_slow");
  context->Update();
  EXPECT_TRUE(shown("chat-picking-line--slow"));
}

} // namespace
} // namespace pbr
