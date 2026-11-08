// The phone chat's bottom: the real chat.rml and composer.rml mounted the way ShellLayoutRml lays out the compact chat
// overlay (pane body, then the composer mount), on the real stylesheets.
#include "gui/chat/ChatController.h"
#include "gui/chat/ChatDataModel.h"
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

std::string Asset(const char* relative) {
  return ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative);
}

constexpr int kWidth = 390;
constexpr int kHeight = 844;

class ChatBottomChromeTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("chat-bottom-chrome-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      ctor.BindEventCallback("send_message", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
        ++sends;
      });
    }));
    const LocalizationService& i18n = LocalizationService::Instance();
    const std::string rml =
        "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/colors-light.rcss") +
        "</style></head><body style=\"padding: 0;\">"
        "<div style=\"position: absolute; top: 0; left: 0; width: " + std::to_string(kWidth) + "dp; height: " +
        std::to_string(kHeight) + "dp;\">"
        "<div class=\"shell-layer shell-layer-base shell-layer-compact\">"
        "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
        "<div class=\"shell-chat-overlay-chrome row surface-chrome\"><div class=\"bar-fade\"></div></div>"
        "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" + i18n.LocalizeText(Asset("views/chat.rml")) + "</div>"
        "<div class=\"shell-composer-mount surface-chrome\" id=\"shell-composer-mount\">" +
        i18n.LocalizeText(Asset("views/composer.rml")) + "</div>"
        "</div></div></div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) { ui::RemoveContext("chat-bottom-chrome-test"); }
    if (initialized) { ui::Shutdown(); }
    ui::SetRenderInterface(nullptr);
  }
  /** The first shown element with class `cls`. */
  ui::Element* ByClass(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    for (ui::Element* element : found) {
      if (element->IsVisible(/*include_ancestors=*/true)) {
        return element;
      }
    }
    return nullptr;
  }
  /** Left, top, right, bottom of the element's border box, in the context's pixels (dp here). */
  struct Edges {
    float left, top, right, bottom;
  };
  static Edges BorderEdges(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    return {at.x, at.y, at.x + size.x, at.y + size.y};
  }
  /** A press and release at the centre of `element`, as a mouse (the focus moves on the press). */
  void Tap(ui::Element* element) {
    const Edges e = BorderEdges(element);
    context->ProcessMouseMove(static_cast<int>((e.left + e.right) / 2), static_cast<int>((e.top + e.bottom) / 2), 0);
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    context->Update();
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
  int sends = 0;
};

// #90 (Kenneth 2026-11-05): sending keeps the keyboard up for the next line; only a tap on the messages closes it.
// The keyboard follows the draft's focus, so: Send leaves the draft focused, a tap on the transcript takes it.
TEST_F(ChatBottomChromeTest, SendKeepsTheDraftFocusedAndATapOnTheMessagesTakesIt) {
  state.draft = "hello";
  DataModelHost::Instance().Dirty("chat", "draft");
  context->Update();
  ui::Element* draft = document->GetElementById("draft-input");
  ASSERT_NE(draft, nullptr);
  ASSERT_TRUE(draft->Focus());
  ui::Element* send = ByClass("prompt-send-btn");
  ASSERT_NE(send, nullptr);
  Tap(send);
  EXPECT_EQ(sends, 1);
  EXPECT_EQ(context->GetFocusElement(), draft) << "Send must not take the focus (the keyboard would drop)";

  Tap(document->GetElementById("chat-messages"));
  EXPECT_NE(context->GetFocusElement(), draft) << "a tap on the messages closes the keyboard";
}

// #89 (Kenneth 2026-11-05): the composer runs edge to edge and down. 2026-11-07: it is a frosted bar over the
// transcript, which runs down under it to the screen bottom; the fade sits right on the bar (KeepClearOfComposer).
TEST_F(ChatBottomChromeTest, TheComposerFloatsOverTheTranscriptEdgeToEdge) {
  ChatController::KeepClearOfComposer(document->GetElementById("shell-chat-overlay"));
  context->Update();
  const Edges overlay = BorderEdges(ByClass("shell-chat-overlay"));
  ui::Element* fade = ByClass("chat-composer-fade");
  ui::Element* mount = document->GetElementById("shell-composer-mount");
  ui::Element* messages = document->GetElementById("chat-messages");
  ASSERT_NE(fade, nullptr);
  ASSERT_NE(mount, nullptr);
  ASSERT_NE(messages, nullptr);
  const Edges f = BorderEdges(fade);
  const Edges m = BorderEdges(mount);
  EXPECT_FLOAT_EQ(f.left, overlay.left);
  EXPECT_FLOAT_EQ(f.right, overlay.right);
  EXPECT_NEAR(f.bottom, m.top, 1.f) << "the fade sits right on the composer";
  EXPECT_FLOAT_EQ(m.left, overlay.left);
  EXPECT_FLOAT_EQ(m.right, overlay.right);
  EXPECT_FLOAT_EQ(m.bottom, overlay.bottom);
  EXPECT_FLOAT_EQ(BorderEdges(messages).bottom, overlay.bottom) << "the transcript runs under the composer";
}

// #89: scrolled to the latest, the last message stood under the composer (the transcript's bottom padding is left out
// of its scroll height by the engine); the spacer at the end, as tall as the bar, keeps it clear, also once the bar
// grows (a long draft).
TEST_F(ChatBottomChromeTest, TheLatestMessageEndsAboveTheComposer) {
  ui::Element* messages = document->GetElementById("chat-messages");
  ui::Element* clear = ByClass("composer-clear");
  ASSERT_NE(messages, nullptr);
  ASSERT_NE(clear, nullptr) << "the transcript's end spacer shows on the phone";
  ui::Element* last = nullptr;
  for (int i = 0; i < 30; ++i) {
    ui::ElementPtr row = document->CreateElement("div");
    row->SetProperty("height", "60dp");
    last = messages->InsertBefore(std::move(row), clear);
  }
  const auto check = [&](const char* when) {
    for (int frame = 0; frame < 2; ++frame) {  // what ChatController::AfterLayout does each frame
      context->Update();
      ChatController::KeepClearOfComposer(document->GetElementById("shell-chat-overlay"));
    }
    context->Update();
    // What ChatTranscriptScroller does for "scroll to the latest".
    messages->SetScrollTop(std::max(0.f, messages->GetScrollHeight() - messages->GetClientHeight()));
    context->Update();
    const Edges bar = BorderEdges(document->GetElementById("shell-composer-mount"));
    EXPECT_LE(BorderEdges(last).bottom, bar.top - 8.f) << when;
  };
  check("one line");
  std::string long_draft;
  for (int line = 0; line < 4; ++line) {
    long_draft += "a line of the draft\n";
  }
  state.draft = long_draft.c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
  check("a five-line draft");
}

// The "new messages" pill sits above the floating composer, not under it.
TEST_F(ChatBottomChromeTest, TheNewMessagesPillSitsAboveTheComposer) {
  state.show_jump_to_latest = true;
  state.jump_to_latest_label = "New messages";
  DataModelHost::Instance().Dirty("chat", "show_jump_to_latest");
  DataModelHost::Instance().Dirty("chat", "jump_to_latest_label");
  context->Update();
  ChatController::KeepClearOfComposer(document->GetElementById("shell-chat-overlay"));
  context->Update();
  ui::Element* pill = ByClass("chat-jump-latest");
  ASSERT_NE(pill, nullptr);
  EXPECT_LE(BorderEdges(pill).bottom, BorderEdges(document->GetElementById("shell-composer-mount")).top - 8.f);
}

} // namespace
} // namespace pbr
