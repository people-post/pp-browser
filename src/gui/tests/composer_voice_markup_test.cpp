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
                               "voice_cancel", "attach_file", "open_emoji_insert", "draft_changed",
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
    document = context->LoadDocumentFromMemory("<rml><head></head><body>" + localized + "</body></rml>");
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
    for (const char* key : {"show_voice", "draft", "loading", "voice_recording", "voice_click_mode"}) {
      DataModelHost::Instance().Dirty("chat", key);
    }
    context->Update();
  }
  /** How many buttons with `cls` are in the composer and shown (a `data-if` that is false hides its element). */
  int Shown(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    int shown = 0;
    for (ui::Element* element : found) {
      shown += element->IsVisible(/*include_ancestors=*/true) ? 1 : 0;
    }
    return shown;
  }
  int ShownSendWithoutMic() {
    ui::ElementList found;
    document->GetElementsByClassName(found, "prompt-send-btn");
    int shown = 0;
    for (ui::Element* element : found) {
      shown += (element->IsVisible(true) && !element->IsClassSet("prompt-voice-btn")) ? 1 : 0;
    }
    return shown;
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ChatScreenState state;
};

TEST_F(ComposerVoiceMarkupTest, MicrophoneTakesTheSendButtonsPlaceWhenVoiceIsOnAndTheDraftIsEmpty) {
  state.show_voice = true;
  state.draft = "";
  state.loading = false;
  state.voice_recording = false;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-btn"), 1);
  EXPECT_EQ(ShownSendWithoutMic(), 0);
}

TEST_F(ComposerVoiceMarkupTest, SendButtonStaysWhenVoiceIsOff) {
  state.show_voice = false;
  state.draft = "";
  state.loading = false;
  state.voice_recording = false;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-btn"), 0);
  EXPECT_EQ(ShownSendWithoutMic(), 1);
}

TEST_F(ComposerVoiceMarkupTest, SendButtonReplacesTheMicrophoneOnceTheDraftHasText) {
  state.show_voice = true;
  state.draft = "hello";
  Apply();
  EXPECT_EQ(Shown("prompt-voice-btn"), 0);
  EXPECT_EQ(ShownSendWithoutMic(), 1);
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

} // namespace
} // namespace pbr
