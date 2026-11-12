// A shell remount (every pushed layer / page sheet runs ShellHost::SyncLayout, which mounts the chat body again) must
// leave a transcript the person scrolled up in where it was. The real chat.rml on the real stylesheets, with enough
// rows to scroll; the remount is the one ShellHost does (RmlMount::MountInner of the pane body, then
// ChatTranscriptScroller::OnShellRemounted, and ApplyPolicy every frame as ChatController::AfterLayout does).
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/RmlMount.h"
#include "gui/shell/ShellBaseHide.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <memory>
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

std::string Asset(const char* relative) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  return text;
}

constexpr int kWidth = 390;
constexpr int kHeight = 844;

class ChatTranscriptRemountTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("chat-transcript-remount-test", {kWidth, kHeight});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      // The scroll event, as ChatController binds it.
      ctor.BindEventCallback("messages_scroll", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
        if (scroller) {
          scroller->OnMessagesScroll();
        }
      });
    }));
    for (int i = 0; i < 80; ++i) {
      MessageDisplayRow row;
      row.message_id = ("m" + std::to_string(i)).c_str();
      row.row_class = "message-row-peer";
      row.content_rml = "<div class=\"bubble\"><p>a message that takes some room</p><p>second line</p></div>";
      row.display_order = i;
      state.messages.push_back(std::move(row));
    }
    state.has_turns = true;
    const std::string chat_body = LocalizationService::Instance().LocalizeText(Asset("views/chat.rml"));
    const std::string rml = "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(kWidth) + "dp; height: " + std::to_string(kHeight) +
                            "dp;\">"
                            "<div class=\"shell-layer shell-layer-base shell-layer-compact\" id=\"shell-base-layer\">"
                            "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">"
                            "<div class=\"shell-pane-body\" id=\"pane-body-chat\">" +
                            chat_body + "</div></div></div></div></body></rml>";
    chat_body_rml = chat_body;
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
    context->Update();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("chat-transcript-remount-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  ui::Element* Messages() { return document->GetElementById("chat-messages"); }
  /** What ShellHost::SyncLayout does to the chat, then the frames that follow it. */
  void Remount() {
    ASSERT_TRUE(RmlMount::MountInner(document->GetElementById("pane-body-chat"), chat_body_rml));
    scroller->OnShellRemounted();
    for (int frame = 0; frame < 6; ++frame) {
      context->Update();
      scroller->ApplyPolicy();
    }
  }
  /** Frames go by: a layout pass, then the scroll policy (ChatController::AfterLayout). */
  void Frames(const int n) {
    for (int frame = 0; frame < n; ++frame) {
      context->Update();
      scroller->ApplyPolicy();
    }
  }
  /** A full-screen call page over the chat hides the base layer (ShellBaseHide), or shows it again. */
  void HideBase(const bool hide) {
    ShellBaseHide::Apply(context, document, hide);
    context->Update();
  }
  void MakeScroller() {
    scroller = std::make_unique<ChatTranscriptScroller>(
        context,
        ChatTranscriptScroller::View{state.show_jump_to_latest, state.jump_to_latest_count, state.messages,
                                     state.has_turns},
        messaging_ready);
  }
  NullRender renderer;
  std::unique_ptr<ChatTranscriptScroller> scroller;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
  bool messaging_ready = true;
  std::string chat_body_rml;
};

TEST_F(ChatTranscriptRemountTest, AScrolledUpTranscriptStaysWhereItWasAfterARemount) {
  MakeScroller();
  ui::Element* messages = Messages();
  ASSERT_NE(messages, nullptr);
  ASSERT_GT(messages->GetScrollHeight() - messages->GetClientHeight(), 2000.f) << "enough rows to scroll";
  messages->SetScrollTop(900.f);
  context->Update();
  scroller->OnMessagesScroll(); // what the scroll event does: not pinned to the bottom any more
  scroller->ApplyPolicy();      // AfterLayout, every frame
  ASSERT_FLOAT_EQ(Messages()->GetScrollTop(), 900.f);

  Remount();
  ASSERT_NE(Messages(), nullptr);
  EXPECT_NEAR(Messages()->GetScrollTop(), 900.f, 1.f) << "the layer opened over the chat moved the transcript";

  // Closing the layer remounts again: still there.
  Remount();
  EXPECT_NEAR(Messages()->GetScrollTop(), 900.f, 1.f);
}

TEST_F(ChatTranscriptRemountTest, ATranscriptPinnedToTheBottomStaysPinnedAfterARemount) {
  MakeScroller();
  ui::Element* messages = Messages();
  ASSERT_NE(messages, nullptr);
  messages->SetScrollTop(messages->GetScrollHeight() - messages->GetClientHeight());
  context->Update();
  scroller->OnMessagesScroll();
  scroller->ApplyPolicy();

  Remount();
  const float max_top = Messages()->GetScrollHeight() - Messages()->GetClientHeight();
  EXPECT_NEAR(Messages()->GetScrollTop(), max_top, 1.f);
}

// DEV/PLAN-call-frame-cost.md step 4: the shell rebuilt during a full-screen call (a turn, a layout change) remounts
// the chat inside the hidden base layer, where nothing is laid out. Its restore must wait for the call to end, not
// spend its frames on a transcript of height 0 and leave the chat at the top after the hang-up.
TEST_F(ChatTranscriptRemountTest, AShellRebuiltDuringAFullScreenCallLeavesTheTranscriptWhereItWas) {
  MakeScroller();
  Messages()->SetScrollTop(900.f);
  context->Update();
  scroller->OnMessagesScroll();
  scroller->ApplyPolicy();
  ASSERT_FLOAT_EQ(Messages()->GetScrollTop(), 900.f);

  HideBase(true);
  Frames(3);
  ASSERT_TRUE(RmlMount::MountInner(document->GetElementById("pane-body-chat"), chat_body_rml));
  scroller->OnShellRemounted();
  Frames(45);      // longer than the restore's 30-frame budget
  HideBase(false); // hung up
  Frames(6);
  ASSERT_NE(Messages(), nullptr);
  EXPECT_NEAR(Messages()->GetScrollTop(), 900.f, 1.f);
  ShellBaseHide::Apply(context, document, false);
}

TEST_F(ChatTranscriptRemountTest, AScrollToTheLatestQueuedWhileHiddenReachesTheBottomOnceShown) {
  MakeScroller();
  Messages()->SetScrollTop(300.f);
  context->Update();
  scroller->OnMessagesScroll();
  scroller->ApplyPolicy();

  HideBase(true);
  Frames(2);
  scroller->RequestScrollToLatest(); // a message sent / arrived while the call page covers the chat
  Frames(80);                        // longer than the 60 attempts it may spend
  HideBase(false);
  Frames(6);
  const float max_top = Messages()->GetScrollHeight() - Messages()->GetClientHeight();
  EXPECT_GT(max_top, 2000.f);
  EXPECT_NEAR(Messages()->GetScrollTop(), max_top, 1.f);
  ShellBaseHide::Apply(context, document, false);
}

} // namespace
} // namespace pbr
