// A sound file's music card in the phone chat (Kenneth 2026-11-06: "播放样式很丑", the time ran past the screen edge):
// the real chat.rml on the real stylesheets at 390dp, rows carrying the card as TranscriptRml draws it.
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/VoiceBubble.h"
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
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
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

class ChatAudioCardLayoutTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    // Real glyph widths, so a long title or time takes its real room.
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context = ui::CreateContext("chat-audio-card-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
    }));
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) { ui::RemoveContext("chat-audio-card-test"); }
    if (initialized) { ui::Shutdown(); }
    ui::SetRenderInterface(nullptr);
  }
  /** The phone chat overlay (as ShellLayoutRml lays it out) with `rows` in its transcript. */
  void Show(std::vector<MessageDisplayRow> rows) {
    state.messages = std::move(rows);
    state.use_messages_layout = true;
    state.has_turns = true;
    const LocalizationService& i18n = LocalizationService::Instance();
    const std::string rml =
        "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
        ReadAll(AssetPath("themes/colors-light.rcss")) +
        "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
        "<div style=\"position: absolute; top: 0; left: 0; width: " + std::to_string(kWidth) + "dp; height: " +
        std::to_string(kHeight) + "dp;\">"
        "<div class=\"shell-layer shell-layer-base shell-layer-compact\">"
        "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
        "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" + i18n.LocalizeText(ReadAll(AssetPath("views/chat.rml"))) +
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

// Received and sent, idle with a long title and playing with the longest minute time: every card stays inside the
// transcript's widest bubble (72% of the row) and the screen, and its time label stays inside the card, right of the bar.
TEST_F(ChatAudioCardLayoutTest, TheCardAndItsTimeStayInsideTheBubbleWidthOnAPhone) {
  const AudioCardFace long_title{
      .title = "A very long song title that would never fit on one line of a phone's chat bubble.mp3",
      .subtitle = "An artist with a long name as well · 10 MB",
      .duration_ms = 59 * 60 * 1000 + 59 * 1000};
  const VoiceBubbleState playing{.playing = true, .paused = false, .position_ms = 59 * 60 * 1000 + 58 * 1000,
                                 .duration_ms = 59 * 60 * 1000 + 59 * 1000};
  Show({Row("a", "message-row-peer", AudioFileBubbleRml("a", "a", long_title, {})),
        Row("b", "message-row-user", AudioFileBubbleRml("b", "b", long_title, playing)),
        Row("c", "message-row-peer", AudioFileBubbleRml("c", "c", AudioCardFace{.title = "x.mp3"}, playing))});
  ui::Element* messages = document->GetElementById("chat-messages");
  ASSERT_NE(messages, nullptr);
  const Edges screen = BorderEdges(messages);
  for (size_t i = 0; i < 3; ++i) {
    SCOPED_TRACE(i);
    ui::Element* card = Nth("chat-audio-card", i);
    ui::Element* time = Nth("chat-audio-time", i);
    ui::Element* track = Nth("chat-voice-track", i);
    ui::Element* title = Nth("chat-audio-title", i);
    ASSERT_NE(card, nullptr);
    ASSERT_NE(time, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(title, nullptr);
    ui::Element* content = card;
    while (content != nullptr && !content->IsClassSet("chat-row-content")) {
      content = content->GetParentNode();
    }
    ASSERT_NE(content, nullptr);
    const Edges c = BorderEdges(card);
    const Edges row = BorderEdges(content);
    EXPECT_GT(c.right - c.left, 150.f) << "laid out as a card";
    EXPECT_LE(c.right - c.left, 0.72f * (row.right - row.left) + 0.5f) << "no wider than the widest bubble";
    EXPECT_GE(c.left, row.left - 0.5f);
    EXPECT_LE(c.right, row.right + 0.5f);
    EXPECT_LE(c.right, screen.right + 0.5f) << "never past the screen edge";
    const Edges t = BorderEdges(time);
    EXPECT_GE(t.left, c.left);
    EXPECT_LE(t.right, c.right) << "the time label is inside the card";
    EXPECT_GE(t.top, c.top);
    EXPECT_LE(t.bottom, c.bottom);
    ui::Element* hit = track->GetParentNode();
    ASSERT_NE(hit, nullptr);
    ASSERT_TRUE(hit->IsClassSet("chat-audio-seek"));
    const Edges area = BorderEdges(hit);
    EXPECT_GE(area.bottom - area.top, 32.f * context->GetDensityIndependentPixelRatio() - 0.5f)
        << "the seek touch area is at least 32dp tall";
    const Edges bar = BorderEdges(track);
    EXPECT_GT(bar.right - bar.left, 20.f) << "the bar keeps room";
    EXPECT_LE(bar.right, t.left) << "the bar ends before the time";
    const Edges name = BorderEdges(title);
    EXPECT_LE(name.right, c.right) << "a long title is cut, not run past the card";
  }
  // The playing time is set as text the label's fixed width holds.
  ui::Element* time = Nth("chat-audio-time", 1);
  EXPECT_EQ(time->GetInnerRML(), "59:58 / 59:59");
  ui::Element* text = time->GetFirstChild();
  ASSERT_NE(text, nullptr);
  EXPECT_LE(BorderEdges(text).right, BorderEdges(time).right + 0.5f) << "\"59:58 / 59:59\" fits its label";
}

// The drag knob: 14dp round, centred on the end of the fill; white on an own (blue) card, dark on a received one.
TEST_F(ChatAudioCardLayoutTest, TheKnobIsARoundSpotAtTheEndOfTheFillWhiteWhenOwnDarkWhenReceived) {
  const VoiceBubbleState half{.playing = true, .paused = false, .position_ms = 50000, .duration_ms = 100000};
  Show({Row("a", "message-row-peer", AudioFileBubbleRml("a", "a", AudioCardFace{.title = "a.mp3"}, half)),
        Row("b", "message-row-user", AudioFileBubbleRml("b", "b", AudioCardFace{.title = "b.mp3"}, half))});
  context->ActivateTheme("light", true);  // the colours live under @media (theme: light)
  context->Update();
  ui::Element* received = Nth("chat-audio-knob", 0);
  ui::Element* own = Nth("chat-audio-knob", 1);
  ASSERT_NE(received, nullptr);
  ASSERT_NE(own, nullptr);
  const float dp = context->GetDensityIndependentPixelRatio();
  for (size_t i = 0; i < 2; ++i) {
    ui::Element* knob = Nth("chat-audio-knob", i);
    const Edges k = BorderEdges(knob);
    EXPECT_NEAR(k.right - k.left, 14.f * dp, 0.5f);
    EXPECT_NEAR(k.bottom - k.top, 14.f * dp, 0.5f);
    const Edges fill = BorderEdges(Nth("chat-voice-fill", i));
    EXPECT_NEAR((k.left + k.right) / 2.f, fill.right, 0.5f) << "centred on the end of the fill";
    const Edges track = BorderEdges(Nth("chat-voice-track", i));
    EXPECT_NEAR((k.top + k.bottom) / 2.f, (track.top + track.bottom) / 2.f, 0.5f) << "centred on the bar";
  }
  EXPECT_EQ(own->GetProperty("background-color")->ToString(), "#ffffff");
  EXPECT_EQ(received->GetProperty("background-color")->ToString(), "#1f2937");
}

} // namespace
} // namespace pbr
