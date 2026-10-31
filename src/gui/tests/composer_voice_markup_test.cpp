#include "gui/chat/ChatDataModel.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

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
  return out.str();
}

// The real composer.rml mounted on the real "chat" data model: the unit tests of VoiceComposer stop at
// ChatScreenState, so a markup / binding fault that hides the microphone button only shows on a device.
class ComposerVoiceMarkupTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("composer-voice-test", {400, 400});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
      BindChatScreenState(ctor, state);
      for (const char* name : {"send_message", "stop_turn", "voice_press", "voice_click", "voice_send",
                               "voice_cancel", "voice_mode_toggle", "attach_file", "open_emoji_insert", "draft_changed",
                               "cancel_quote_reply", "retry_attachment_send", "discard_attachment_send",
                               "remove_image"}) {
        ctor.BindEventCallback(name, [](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {});
      }
    }));
    std::ifstream file(std::string(PP_BROWSER_ASSETS_DIR) + "/views/composer.rml");
    ASSERT_TRUE(file.good());
    std::ostringstream body;
    body << file.rdbuf();
    const auto localized = LocalizationService::Instance().LocalizeText(body.str());
    document = context->LoadDocumentFromMemory("<rml><head><style>" + ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/themes/components.rcss") +
                                              "</style></head><body>" + localized + "</body></rml>");
    ASSERT_NE(document, nullptr);
    document->Show();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) { ui::RemoveContext("composer-voice-test"); }
    if (initialized) { ui::Shutdown(); }
    ui::SetRenderInterface(nullptr);
  }
  void Apply() {
    for (const char* key : {"show_voice", "voice_mode", "draft", "loading", "voice_recording", "voice_click_mode"}) {
      DataModelHost::Instance().Dirty("chat", key);
    }
    context->Update();
  }
  /** How many elements with `cls` are in the composer and shown (a `data-if` that is false hides its element). */
  int Shown(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    int shown = 0;
    for (ui::Element* element : found) {
      shown += element->IsVisible(/*include_ancestors=*/true) ? 1 : 0;
    }
    return shown;
  }
  /** The text input is in the layout (its `data-if` is true). */
  bool InputShown() {
    ui::Element* input = document->GetElementById("draft-input");
    return input && input->IsVisible(true);
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

// WeChat model: a toggle at the left of the footer switches the text input for a "hold to talk" bar.
TEST_F(ComposerVoiceMarkupTest, TextModeShowsTheToggleTheInputAndSendButNoBar) {
  state.show_voice = true;
  state.voice_mode = false;
  state.draft = "";
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1);
  EXPECT_EQ(Shown("prompt-voice-bar"), 0);
  EXPECT_TRUE(InputShown());
  EXPECT_EQ(Shown("prompt-send-btn"), 1);
}

TEST_F(ComposerVoiceMarkupTest, VoiceModeShowsTheBarAndTheToggleButNoInputOrSend) {
  state.show_voice = true;
  state.voice_mode = true;
  state.draft = "";
  Apply();
  EXPECT_EQ(Shown("prompt-voice-bar"), 1);
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1); // the keyboard one, to go back
  EXPECT_FALSE(InputShown());
  EXPECT_EQ(Shown("prompt-send-btn"), 0);
}

TEST_F(ComposerVoiceMarkupTest, TheBarSaysHoldOnAPhoneAndClickOnTheDesktop) {
  state.show_voice = true;
  state.voice_mode = true;
  state.voice_click_mode = false;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-bar-hold"), 1);
  EXPECT_EQ(Shown("prompt-voice-bar-click"), 0);
  state.voice_click_mode = true;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-bar-hold"), 0);
  EXPECT_EQ(Shown("prompt-voice-bar-click"), 1);
}

TEST_F(ComposerVoiceMarkupTest, NoVoiceHidesTheToggleAndTheBar) {
  state.show_voice = false;
  state.voice_mode = false;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 0);
  EXPECT_EQ(Shown("prompt-voice-bar"), 0);
  EXPECT_TRUE(InputShown());
  EXPECT_EQ(Shown("prompt-send-btn"), 1);
}

TEST_F(ComposerVoiceMarkupTest, TheToggleStaysWhenTheDraftHasText) {
  state.show_voice = true;
  state.voice_mode = false;
  state.draft = "hello";
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1);
  EXPECT_EQ(Shown("prompt-send-btn"), 1);
}

TEST_F(ComposerVoiceMarkupTest, WhileRecordingOnTheDesktopTheBarStaysAndCancelAndSendAppear) {
  state.show_voice = true;
  state.voice_mode = true;
  state.voice_click_mode = true;
  state.voice_recording = true;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-bar"), 1);
  EXPECT_EQ(Shown("prompt-voice-cancel-btn"), 1);
  EXPECT_EQ(Shown("prompt-send-btn"), 1); // the voice Send
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 0);
}

// Reported on a device: after typing and sending, the toggle was gone until the chat was reopened.
TEST_F(ComposerVoiceMarkupTest, TheToggleKeepsItsSizeWhileTypingALongDraftAndAfterSending) {
  state.show_voice = true;
  state.voice_mode = false;
  state.draft = "";
  Apply();
  const auto toggle_width = [this]() {
    ui::ElementList found;
    document->GetElementsByClassName(found, "prompt-voice-toggle-btn");
    float width = 0;
    for (ui::Element* element : found) {
      if (element->IsVisible(true)) {
        width = element->GetClientWidth();
      }
    }
    return width;
  };
  const float before = toggle_width();
  EXPECT_FLOAT_EQ(before, 32.f);
  std::string long_draft;
  for (int line = 0; line < 12; ++line) {
    long_draft += "a long line of text that wraps over the width of the composer when typed, number " +
                  std::to_string(line) + "\n";
  }
  state.draft = long_draft.c_str();
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1);
  EXPECT_FLOAT_EQ(toggle_width(), before);
  // Send: the draft is cleared, the reply starts (loading) and ends.
  state.draft = "";
  state.loading = true;
  Apply();
  state.loading = false;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1);
  EXPECT_FLOAT_EQ(toggle_width(), before);
  // The shell may mount the composer again (RmlMount::MountInner) while the chat stays open.
  document->SetInnerRML(ui::String(LocalizationService::Instance().LocalizeText(ReadAll(std::string(PP_BROWSER_ASSETS_DIR) + "/views/composer.rml")).c_str()));
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1);
  EXPECT_FLOAT_EQ(toggle_width(), before);
}

// The cause of the missing microphone on devices: the markup above is fine, but `show_voice` is only set by
// VoiceComposer::Refresh(), and Refresh() sat in ChatController::UpdateThreadChrome(), which nothing called.
// The thread chrome (and messaging_ready_ / mesh_ready_ with it) changed through `chrome_.Update()` alone, so
// show_voice stayed false. A ChatController cannot be built in a unit test, so this reads the source.
TEST(ComposerVoiceSourceTest, EveryThreadChromeUpdateGoesThroughUpdateThreadChrome) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  const std::string source = out.str();
  ASSERT_FALSE(source.empty()) << "ChatController.cpp not found under PP_SOURCE_DIR";
  size_t direct = 0;
  for (size_t at = source.find("chrome_.Update();"); at != std::string::npos;
       at = source.find("chrome_.Update();", at + 1)) {
    ++direct;
  }
  EXPECT_EQ(direct, 1u) << "chrome_.Update() belongs only inside UpdateThreadChrome(), which also refreshes the "
                           "microphone button";
  size_t calls = 0;
  for (size_t at = source.find("UpdateThreadChrome();"); at != std::string::npos;
       at = source.find("UpdateThreadChrome();", at + 1)) {
    ++calls;
  }
  EXPECT_GE(calls, 3u) << "RefreshFromMessaging, OnMessagingReady and OnMeshReady must call UpdateThreadChrome()";
}

// Eligibility for a voice message includes "no upload running" (ChatController.cpp voice_ports.eligible). The
// toggle goes away while a file is sent; every place that ends the upload calls SyncComposerInputState(), so that
// is where the toggle is recomputed. Reported on a device: the toggle stayed gone after sending until the chat was
// reopened.
TEST(ComposerVoiceSourceTest, SyncComposerInputStateRefreshesTheVoiceToggle) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  const std::string source = out.str();
  const size_t start = source.find("void ChatController::SyncComposerInputState() {");
  ASSERT_NE(start, std::string::npos);
  const size_t end = source.find("\n}\n", start);
  ASSERT_NE(end, std::string::npos);
  EXPECT_NE(source.substr(start, end - start).find("voice_->Refresh()"), std::string::npos);
}

} // namespace
} // namespace pbr
