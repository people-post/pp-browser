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
      if (video_reaction && message.id == "v") {
        row.reactions.push_back({"\xF0\x9F\x91\x8D", 2, false, {"a", "b"}});
      }
      rows.push_back(row);
    }
    TranscriptFacts facts;
    facts.voice_supported = true;
    facts.audio_files_play = true;
    facts.send_mark = [mark](const ThreadMessage&) { return mark; };
    facts.attachment = [this](const ThreadMessage& message) {
      TranscriptAttachmentView view;
      view.state = AttachmentFetchWorkflow::DownloadState::Ready;
      view.inline_image_ok = true;
      view.image_width = 4032;
      view.image_height = 3024;
      if (message.id == "p") {
        view.local_path = "/x/a.jpg";
        if (picture_w > 0) {
          view.image_width = picture_w;
          view.image_height = picture_h;
        }
      } else if (message.id == "v") {
        view.poster_path = video_poster ? "/x/poster.jpg" : "";
        if (video_w > 0) {
          view.image_width = video_w;
          view.image_height = video_h;
        }
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

  // What the video of ShowAll looks like (poster or the bare frame; the poster's size, 0: the default).
  bool video_poster = true;
  int picture_w = 0;
  int picture_h = 0;
  bool video_reaction = false;
  int video_w = 0;
  int video_h = 0;

  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

// The selectors of each kind's time, text bubble first.
const char* const kTimeSelectors[] = {
    ".bubble .bubble-time",                         // text
    ".chat-attachment-card--image .chat-foot-time", // picture: the footer row under it
    ".chat-attachment-card--video .chat-foot-time", // video: the footer row under its frame
    ".chat-file-card .chat-attachment-clock",       // file
    ".chat-audio-card .chat-attachment-clock",      // sound file
    ".chat-voice-clock",                            // voice message
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

struct Box {
  float left, top, right, bottom;
};

Box BoxOf(ui::Element* element) {
  const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
  return {at.x, at.y, at.x + size.x, at.y + size.y};
}

bool Overlaps(const Box& a, const Box& b) {
  return a.left < b.right - 0.5f && b.left < a.right - 0.5f && a.top < b.bottom - 0.5f && b.top < a.bottom - 0.5f;
}

// A picture's or video's time is a slim row under the media inside the bubble: nothing is drawn over the media for it,
// the row sits below the media, right-aligned, and the bubble is as wide as the media (plus its 3dp frame).
TEST_P(AttachmentTimeTest, ThePictureAndVideoTimeIsARowUnderTheMediaInsideTheBubbleAndNothingCoversTheMedia) {
  ShowAll(kSentAt, SendMark::None);
  EXPECT_EQ(All(".chat-attachment-time").size(), 0U);
  EXPECT_EQ(All(".chat-attachment-pill").size(), 0U);
  for (const char* parent : {".chat-attachment-card--image", ".chat-attachment-card--video"}) {
    SCOPED_TRACE(parent);
    const bool video = std::string(parent) == ".chat-attachment-card--video";
    ui::Element* card = All(parent).front();
    ui::Element* media = All(video ? ".chat-attachment-video-frame" : ".chat-attachment-clip").front();
    ui::Element* footer = All((std::string(parent) + " .chat-attachment-foot").c_str()).front();
    ui::Element* time = TimeIn((std::string(parent) + " .chat-foot-time").c_str());
    ASSERT_NE(time, nullptr);
    const Box card_box = BoxOf(card);
    const Box media_box = BoxOf(media);
    const Box foot_box = BoxOf(footer);
    const Box time_box = BoxOf(time);
    EXPECT_GE(foot_box.top, media_box.bottom - 0.5f) << "under the media";
    EXPECT_FALSE(Overlaps(time_box, media_box)) << "nothing is drawn over the media";
    EXPECT_FALSE(Overlaps(foot_box, media_box));
    EXPECT_GE(foot_box.left, card_box.left - 0.5f) << "inside the bubble";
    EXPECT_LE(foot_box.right, card_box.right + 0.5f);
    EXPECT_LE(foot_box.bottom, card_box.bottom + 0.5f);
    EXPECT_NEAR(card_box.bottom - foot_box.bottom, 3.f, 0.5f) << "the card ends 3dp under the row";
    EXPECT_GT(time_box.left, (foot_box.left + foot_box.right) / 2.f) << "right-aligned";
    EXPECT_NEAR(card_box.right - card_box.left, (media_box.right - media_box.left) + 6.f, 0.5f)
        << "the bubble hugs the media";
    EXPECT_LE(foot_box.bottom - foot_box.top, 14.f + 6.f + 0.5f) << "about one line";
  }
}

TEST_P(AttachmentTimeTest, TheVideoFooterReadsSizeDotIconTimeAndIsTheOnlyRowUnderTheFrame) {
  ShowAll(kSentAt, SendMark::None);
  ui::Element* footer = All(".chat-attachment-card--video .chat-attachment-foot").front();
  const std::string text = footer->GetInnerRML().c_str();
  ASSERT_NE(text.find("KB"), std::string::npos) << "the size";
  EXPECT_NE(text.find("\xC2\xB7"), std::string::npos) << "the middle dot";
  EXPECT_LT(text.find("KB"), text.find("route-icon")) << "size, then the icon";
  EXPECT_LT(text.find("route-icon"), text.find(':')) << "then the time";
  EXPECT_EQ(All(".chat-attachment-card--video .muted").size(), 0U) << "no second row under the frame";
  EXPECT_EQ(All(".chat-attachment-card--video .chat-attachment-foot").size(), 1U);
}

TEST_P(AttachmentTimeTest, APictureFooterHasTheTimeAndNoSize) {
  ShowAll(kSentAt, SendMark::None);
  ui::Element* footer = All(".chat-attachment-card--image .chat-attachment-foot").front();
  const std::string text = footer->GetInnerRML().c_str();
  EXPECT_EQ(text.find("KB"), std::string::npos);
  EXPECT_EQ(text.find("MB"), std::string::npos);
  EXPECT_EQ(text.find("\xC2\xB7"), std::string::npos);
  EXPECT_NE(text.find(':'), std::string::npos);
}

TEST_P(AttachmentTimeTest, TheFooterUsesTheTextBubblesTimeColours) {
  ShowAll(kSentAt, SendMark::None);
  ui::Element* text_time = TimeIn(".bubble .bubble-time");
  ASSERT_NE(text_time, nullptr);
  const ui::Colourb expected = text_time->GetComputedValues().color();
  for (const char* selector :
       {".chat-attachment-card--image .chat-attachment-foot", ".chat-attachment-card--video .chat-attachment-foot",
        ".chat-file-card .chat-attachment-clock"}) {
    SCOPED_TRACE(selector);
    const ui::Colourb colour = All(selector).front()->GetComputedValues().color();
    EXPECT_EQ(colour.red, expected.red);
    EXPECT_EQ(colour.green, expected.green);
    EXPECT_EQ(colour.blue, expected.blue);
    EXPECT_EQ(colour.alpha, expected.alpha);
  }
}

TEST_P(AttachmentTimeTest, TheFileAndSoundCardsTimeIsAtTheirBottomRightInsideThem) {
  ShowAll(kSentAt, SendMark::None);
  for (const char* parent : {".chat-file-card", ".chat-audio-card"}) {
    SCOPED_TRACE(parent);
    ui::Element* time = TimeIn((std::string(parent) + " .chat-attachment-clock").c_str());
    ASSERT_NE(time, nullptr);
    const Box card = BoxOf(All(parent).front());
    const Box box = BoxOf(time);
    EXPECT_LE(box.right, card.right + 0.5f);
    EXPECT_LE(box.bottom, card.bottom + 0.5f);
    EXPECT_GT(box.top, (card.top + card.bottom) / 2.f) << "at the bottom";
  }
}

// Reactions hang over the bubble's bottom edge at its left; the footer's time is at the right: they do not meet.
TEST_P(AttachmentTimeTest, TheReactionsOfAVideoDoNotCoverItsTime) {
  video_reaction = true;
  ShowAll(kSentAt, SendMark::None);
  ui::Element* chip = nullptr;
  for (ui::Element* candidate : All(".chat-reaction-chip")) {
    if (Shown(candidate)) {
      chip = candidate;
      break;
    }
  }
  ASSERT_NE(chip, nullptr);
  ui::Element* time = TimeIn(".chat-attachment-card--video .chat-foot-time");
  ASSERT_NE(time, nullptr);
  EXPECT_LE(BoxOf(chip).right, BoxOf(time).left + 0.5f) << "the pills sit left of the time";
  ui::Element* frame = All(".chat-attachment-video-frame").front();
  EXPECT_GE(BoxOf(chip).top + 12.f, BoxOf(frame).bottom) << "and under the frame, not over the picture";
}

TEST_P(AttachmentTimeTest, ANarrowVideoFrameDropsTheSizeAndKeepsTheTimeAndTheBubbleHugsIt) {
  video_w = 90;
  video_h = 1600; // a very tall video: a narrow frame
  ShowAll(kSentAt, SendMark::None);
  ui::Element* frame = All(".chat-attachment-video-frame").front();
  ASSERT_LT(frame->GetBox().GetSize(ui::BoxArea::Border).x, 150.f) << "the frame is narrow";
  ui::Element* footer = All(".chat-attachment-card--video .chat-attachment-foot").front();
  const std::string text = footer->GetInnerRML().c_str();
  EXPECT_EQ(text.find("KB"), std::string::npos) << "the size is the one that goes";
  EXPECT_EQ(text.find("\xC2\xB7"), std::string::npos) << "and its separator";
  EXPECT_NE(TimeIn(".chat-attachment-card--video .chat-foot-time"), nullptr) << "the time stays";
  EXPECT_NEAR(BoxOf(All(".chat-attachment-card--video").front()).right -
                  BoxOf(All(".chat-attachment-card--video").front()).left,
              BoxOf(frame).right - BoxOf(frame).left + 6.f, 0.5f);
}

TEST_P(AttachmentTimeTest, ATallNarrowAndAWidePictureBothKeepTheirFooterInsideAMediaWideBubble) {
  for (const auto& dims : {std::pair<int, int>{400, 2000}, std::pair<int, int>{4000, 1000}}) {
    SCOPED_TRACE(std::to_string(dims.first) + "x" + std::to_string(dims.second));
    picture_w = dims.first;
    picture_h = dims.second;
    ShowAll(kSentAt, SendMark::None);
    ui::Element* card = All(".chat-attachment-card--image").front();
    ui::Element* clip = All(".chat-attachment-clip").front();
    ui::Element* footer = All(".chat-attachment-card--image .chat-attachment-foot").front();
    ui::Element* time = TimeIn(".chat-attachment-card--image .chat-foot-time");
    ASSERT_NE(time, nullptr);
    EXPECT_NEAR(BoxOf(card).right - BoxOf(card).left, BoxOf(clip).right - BoxOf(clip).left + 6.f, 0.5f);
    EXPECT_LE(BoxOf(time).right, BoxOf(footer).right + 0.5f);
    EXPECT_GE(BoxOf(time).left, BoxOf(footer).left - 0.5f);
    context->UnloadDocument(document);
    document = nullptr;
  }
}

TEST_P(AttachmentTimeTest, TheBareVideoFramesLabelAndNameAreWhiteOnOneLineAndItsFooterHasTheSize) {
  video_poster = false;
  ShowAll(kSentAt, SendMark::None);
  for (const char* selector : {".chat-attachment-video-badge", ".chat-attachment-video-name"}) {
    SCOPED_TRACE(selector);
    ui::Element* element = All(selector).front();
    const ui::Colourb colour = element->GetComputedValues().color();
    EXPECT_GE(colour.red, 250);
    EXPECT_GE(colour.green, 250);
    EXPECT_GE(colour.blue, 250);
  }
  ui::Element* name = All(".chat-attachment-video-name").front();
  EXPECT_LE(name->GetBox().GetSize(ui::BoxArea::Border).y, 24.f) << "one line";
  ui::Element* footer = All(".chat-attachment-card--video .chat-attachment-foot").front();
  EXPECT_NE(std::string(footer->GetInnerRML().c_str()).find("KB"), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(ThemesAndSides, AttachmentTimeTest,
                         testing::Values(Case{"light", true}, Case{"light", false}, Case{"dark", true},
                                         Case{"dark", false}));

} // namespace
} // namespace pbr
