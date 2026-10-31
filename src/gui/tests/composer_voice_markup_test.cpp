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
                               "remove_image", "attach_capture"}) {
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
    for (const char* key : {"show_voice", "voice_mode", "draft", "loading", "voice_recording", "voice_click_mode",
                            "show_attach_button", "show_capture", "attachment_uploading", "attachment_failed",
                            "attachment_draft_name", "attachment_status", "image_chip", "image_thumb_ready",
                            "image_preparing", "voice_blocked", "voice_countdown", "voice_cancel_zone"}) {
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
  /** Some shown element with class `cls` has an ancestor with class `ancestor_cls`. */
  bool InsideClass(const char* cls, const char* ancestor_cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    for (ui::Element* element : found) {
      if (!element->IsVisible(true)) {
        continue;
      }
      for (ui::Element* up = element->GetParentNode(); up; up = up->GetParentNode()) {
        if (up->IsClassSet(ancestor_cls)) {
          return true;
        }
      }
    }
    return false;
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

// Layout (Kenneth 2026-10-31): [toggle] [emoji] [pill: input | clip | camera] [send, only with text]; voice mode
// [toggle] [bar]; a status row above the pill only when something needs saying.
TEST_F(ComposerVoiceMarkupTest, TextModeWithoutADraftShowsTheToggleThePillAndNoSend) {
  state.show_voice = true;
  state.show_attach_button = true;
  state.voice_mode = false;
  state.draft = "";
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1);
  EXPECT_EQ(Shown("prompt-voice-bar"), 0);
  EXPECT_EQ(Shown("prompt-composer-pill"), 1);
  EXPECT_TRUE(InputShown());
  EXPECT_EQ(Shown("prompt-attach-btn"), 1);
  EXPECT_EQ(Shown("prompt-send-btn"), 0);
  EXPECT_EQ(Shown("prompt-composer-status"), 0) << "nothing to say: no status row";
}

TEST_F(ComposerVoiceMarkupTest, ADraftBringsTheSendButton) {
  state.show_voice = true;
  state.show_attach_button = true;
  state.draft = "hello";
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1);
  EXPECT_EQ(Shown("prompt-send-btn"), 1);
  state.loading = true;
  Apply();
  EXPECT_EQ(Shown("prompt-send-btn"), 0) << "an AI answer is running: the stop key replaces Send";
  EXPECT_EQ(Shown("prompt-stop-btn"), 1);
  EXPECT_EQ(Shown("prompt-composer-status"), 1);
}

TEST_F(ComposerVoiceMarkupTest, VoiceModeShowsTheBarAndTheToggleOnly) {
  state.show_voice = true;
  state.show_attach_button = true;
  state.show_capture = true;
  state.voice_mode = true;
  state.draft = "left over";
  Apply();
  EXPECT_EQ(Shown("prompt-voice-bar"), 1);
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 1); // the keyboard one, to go back
  EXPECT_EQ(Shown("prompt-composer-pill"), 0);
  EXPECT_FALSE(InputShown());
  EXPECT_EQ(Shown("prompt-attach-btn"), 0);
  EXPECT_EQ(Shown("prompt-capture-btn"), 0);
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
  state.show_attach_button = true;
  state.voice_mode = false;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 0);
  EXPECT_EQ(Shown("prompt-voice-bar"), 0);
  EXPECT_TRUE(InputShown());
  EXPECT_EQ(Shown("prompt-send-btn"), 0);
}

TEST_F(ComposerVoiceMarkupTest, RecordingOnAPhoneShowsDotTimerAndHintInsideTheBar) {
  state.show_voice = true;
  state.voice_mode = true;
  state.voice_click_mode = false;
  state.voice_recording = true;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-bar"), 1);
  EXPECT_EQ(Shown("prompt-voice-status"), 1);
  EXPECT_TRUE(InsideClass("prompt-voice-status", "prompt-voice-bar"));
  EXPECT_EQ(Shown("prompt-voice-dot"), 1);
  EXPECT_EQ(Shown("prompt-voice-timer"), 1);
  EXPECT_EQ(Shown("prompt-voice-hint"), 1);
  EXPECT_EQ(Shown("prompt-voice-bar-hold"), 0) << "the status takes the label's place";
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 0);
  EXPECT_EQ(Shown("prompt-voice-cancel-btn"), 0);
  EXPECT_EQ(Shown("prompt-send-btn"), 0);
}

TEST_F(ComposerVoiceMarkupTest, RecordingOnTheDesktopAddsCancelAndSendBesideTheBar) {
  state.show_voice = true;
  state.voice_mode = true;
  state.voice_click_mode = true;
  state.voice_recording = true;
  Apply();
  EXPECT_EQ(Shown("prompt-voice-bar"), 1);
  EXPECT_EQ(Shown("prompt-voice-status"), 1);
  EXPECT_EQ(Shown("prompt-voice-cancel-btn"), 1);
  EXPECT_EQ(Shown("prompt-send-btn"), 1); // the voice Send
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 0);
}

// The Retry / Cancel of a failed send (and the voice recording kept for retry) must stay reachable.
TEST_F(ComposerVoiceMarkupTest, AFailedAttachmentShowsRetryAndCancelInTheStatusRow) {
  state.show_voice = true;
  state.show_attach_button = true;
  state.attachment_failed = true;
  state.attachment_draft_name = "clip.mp4";
  state.attachment_status = "Not sent";
  Apply();
  EXPECT_EQ(Shown("prompt-composer-status"), 1);
  EXPECT_EQ(Shown("prompt-attachment-chip"), 1);
  EXPECT_TRUE(InsideClass("prompt-attachment-chip", "prompt-composer-status"));
  EXPECT_EQ(Shown("prompt-attachment-chip-action"), 2);
  EXPECT_TRUE(InsideClass("prompt-attachment-chip-action", "prompt-composer-status"));
}

TEST_F(ComposerVoiceMarkupTest, AnUploadInProgressShowsTheChipWithoutRetry) {
  state.attachment_uploading = true;
  state.attachment_draft_name = "clip.mp4";
  Apply();
  EXPECT_EQ(Shown("prompt-composer-status"), 1);
  EXPECT_EQ(Shown("prompt-attachment-chip"), 1);
  EXPECT_EQ(Shown("prompt-attachment-chip-action"), 0);
}

TEST_F(ComposerVoiceMarkupTest, TheAiPictureThumbnailSitsInTheStatusRow) {
  state.show_attach_button = true;
  state.image_chip = true;
  state.image_thumb_ready = false;
  Apply();
  EXPECT_EQ(Shown("prompt-composer-status"), 1);
  EXPECT_EQ(Shown("prompt-image-thumb-wrap"), 1);
  EXPECT_TRUE(InsideClass("prompt-image-thumb-wrap", "prompt-composer-status"));
  EXPECT_EQ(Shown("prompt-image-remove-badge"), 1);
  state.image_chip = false;
  Apply();
  EXPECT_EQ(Shown("prompt-composer-status"), 0);
}

TEST_F(ComposerVoiceMarkupTest, TheCameraFollowsShowCaptureAndSitsInThePill) {
  state.show_voice = true;
  state.show_attach_button = true;
  state.show_capture = true;
  Apply();
  EXPECT_EQ(Shown("prompt-capture-btn"), 1);
  EXPECT_TRUE(InsideClass("prompt-capture-btn", "prompt-composer-pill"));
  EXPECT_TRUE(InsideClass("prompt-attach-btn", "prompt-composer-pill"));
  state.show_capture = false;
  Apply();
  EXPECT_EQ(Shown("prompt-capture-btn"), 0);
  EXPECT_EQ(Shown("prompt-attach-btn"), 1) << "the paperclip does not depend on the camera";
}

TEST_F(ComposerVoiceMarkupTest, TheCameraGoesWhileRecording) {
  state.show_voice = true;
  state.show_attach_button = true;
  state.show_capture = true;
  state.voice_mode = false;
  state.voice_recording = true; // e.g. a hold that started and the mode flag not yet flipped
  Apply();
  EXPECT_EQ(Shown("prompt-capture-btn"), 0);
}

// An AI composer: the paperclip alone (the AI picture flow), no camera, no microphone toggle.
TEST_F(ComposerVoiceMarkupTest, AnAiComposerHasOnlyThePaperclip) {
  state.show_voice = false;
  state.show_capture = false;
  state.show_attach_button = true;
  state.draft = "what is this?";
  Apply();
  EXPECT_EQ(Shown("prompt-attach-btn"), 1);
  EXPECT_EQ(Shown("prompt-capture-btn"), 0);
  EXPECT_EQ(Shown("prompt-voice-toggle-btn"), 0);
  EXPECT_EQ(Shown("prompt-send-btn"), 1);
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


// The camera icon follows the same state as the toggle: it must be recomputed where an upload ends (Sync...), where a
// call starts or ends (UpdatePeerLinkChrome) and with the thread chrome (which ends in Sync...).
TEST(ComposerCaptureSourceTest, ShowCaptureIsRecomputedWhereTheComposerStateChanges) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  const std::string source = out.str();
  const auto body_of = [&source](const std::string& signature) {
    const size_t start = source.find(signature);
    EXPECT_NE(start, std::string::npos) << signature;
    const size_t end = source.find("\n}\n", start);
    return start == std::string::npos ? std::string() : source.substr(start, end - start);
  };
  EXPECT_NE(body_of("void ChatController::SyncComposerInputState() {").find("RefreshShowCapture()"), std::string::npos);
  EXPECT_NE(body_of("void ChatController::UpdatePeerLinkChrome() {").find("RefreshShowCapture()"), std::string::npos);
  EXPECT_NE(body_of("void ChatController::UpdateThreadChrome() {").find("SyncComposerInputState()"), std::string::npos);
  EXPECT_NE(body_of("void ChatController::RefreshShowCapture() {").find("ShowCaptureEntry("), std::string::npos);
}

// A picked / captured file is released by release_pick_ (injectable), never by a direct ReleasePickedFile call, and
// StartAttachmentUpload releases it on both early returns too (a camera file would otherwise stay on disk).
TEST(ComposerCaptureSourceTest, StartAttachmentUploadReleasesTheFileOnEveryExit) {
  std::ifstream in(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp", std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  const std::string source = out.str();
  const size_t start = source.find("void ChatController::StartAttachmentUpload(");
  ASSERT_NE(start, std::string::npos);
  const size_t end = source.find("\nbool ChatController::InAiComposerContext()", start);
  ASSERT_NE(end, std::string::npos);
  const std::string body = source.substr(start, end - start);
  EXPECT_EQ(body.find("ReleasePickedFile("), std::string::npos) << "use release_pick_";
  const auto count = [&body](const std::string& needle) {
    size_t n = 0;
    for (size_t at = body.find(needle); at != std::string::npos; at = body.find(needle, at + 1)) {
      ++n;
    }
    return n;
  };
  EXPECT_EQ(count("release_pick_(path);"), 3u) << "two early returns and the worker's own release";
  // The early returns release before they return.
  EXPECT_NE(body.find("chat_.attachment_uploading) {\n    release_pick_(path);\n    return;"), std::string::npos);
  EXPECT_NE(body.find("thread_id.empty()) {\n    release_pick_(path);\n    return;"), std::string::npos);
}

} // namespace
} // namespace pbr
