// DEV/PLAN-desktop-drop-paste.md, D1: the two ways a paste reaches the chat on a computer. The key binding (Ctrl+V; Cmd+V
// on a Mac) and the menu interceptor must act ONLY in the chat's own message box — never in Home's or an AI's box, the
// AI half-panel, a search or settings field, or while a message is edited — and a key held down must pass through
// untouched. Whatever they do not take reaches the text field exactly as before. A fake clipboard stands in for the
// system's; the real one is never touched. The ChatController wiring is read from its source (it cannot be built here).
#include "feature/presenters/AttachIntake.h"
#include "foundation/platform/ClipboardAttachments.h"
#include "foundation/platform/Platform.h"
#include "gui/chat/PasteTrigger.h"
#include "lib/ui/InputCoordinator.h"

#include <ui/base/Input.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <optional>
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
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class PasteTriggerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("paste-trigger-test", {400, 400});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(
        "<rml><head><style>body { display: block; width: 400px; height: 400px; } "
        "textarea, input { display: block; width: 200px; height: 30px; }</style></head><body>"
        "<textarea id='draft-input'></textarea><textarea id='ask-ai-input'></textarea>"
        "<input id='search-input' type='text'/><input class='field' type='text'/><textarea id='ps-text'></textarea>"
        "</body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();

    // The clipboard the paste would read: a picture alone. Touching it is counted.
    clipboard_.mime_types = [this]() {
      ++clipboard_touches_;
      return std::vector<std::string>{"image/png"};
    };
    clipboard_.text = [this]() {
      ++clipboard_touches_;
      return std::string();
    };
    clipboard_.data = [this](const std::string&) {
      ++clipboard_touches_;
      return std::optional<std::string>();
    };
    SetClipboardSourceForTest(clipboard_);

    ports_.in_chat_composer_context = [this]() { return chat_context_; };
    ports_.paste = [this]() {
      ++paste_calls_;
      PeekClipboard(); // what the real paste does first
      return paste_result_;
    };
  }

  void TearDown() override {
    SetClipboardSourceForTest(std::nullopt);
    if (context_) {
      ui::RemoveContext("paste-trigger-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void Focus(const char* id) {
    ui::Element* element = document_->GetElementById(id);
    ASSERT_NE(element, nullptr) << id;
    element->Focus();
    ASSERT_EQ(context_->GetFocusElement(), element) << id;
  }
  /** What ProcessKeyDown answers: false = the binding took the key. */
  bool KeyV(int modifiers, bool repeat = false, bool priority_phase = true) {
    InputCoordinator input;
    input.Register(PasteKeyBinding(ports_, ui::Input::KM_CTRL));
    return input.ProcessKeyDown(context_, ui::Input::KI_V, modifiers, priority_phase, repeat);
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ClipboardSource clipboard_;
  int clipboard_touches_ = 0;
  PasteTriggerPorts ports_;
  bool chat_context_ = true;
  bool paste_result_ = true;
  int paste_calls_ = 0;
};

TEST_F(PasteTriggerTest, CtrlVInTheChatBoxTakesTheKeyWhenThePasteIsHandled) {
  Focus("draft-input");
  EXPECT_FALSE(KeyV(ui::Input::KM_CTRL)) << "taken: the text field never sees it";
  EXPECT_EQ(paste_calls_, 1);
}

TEST_F(PasteTriggerTest, WhenThePasteIsNotHandledTheKeyGoesOnToTheTextField) {
  Focus("draft-input");
  paste_result_ = false;
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL));
  EXPECT_EQ(paste_calls_, 1);
}

TEST_F(PasteTriggerTest, AKeyHeldDownPassesThroughWithoutTouchingTheClipboard) {
  Focus("draft-input");
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL, /*repeat=*/true));
  EXPECT_EQ(paste_calls_, 0);
  EXPECT_EQ(clipboard_touches_, 0);
}

TEST_F(PasteTriggerTest, OtherFieldsAreNeverTouchedAndTheClipboardStaysUnread) {
  for (const char* id : {"ask-ai-input", "search-input", "ps-text"}) {
    Focus(id);
    EXPECT_TRUE(KeyV(ui::Input::KM_CTRL)) << id;
  }
  context_->GetDocument(0)->QuerySelector(".field")->Focus();
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL)) << "a settings-style field";
  EXPECT_EQ(paste_calls_, 0);
  EXPECT_EQ(clipboard_touches_, 0);
}

TEST_F(PasteTriggerTest, TheSharedBoxIdOfHomeAnAiConversationOrAnEditIsLeftAlone) {
  Focus("draft-input");
  chat_context_ = false; // Home's or an AI's box, or a message being edited: same id, not the chat's
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL));
  EXPECT_EQ(paste_calls_, 0);
  EXPECT_EQ(clipboard_touches_, 0);
}

TEST_F(PasteTriggerTest, NothingFocusedIsLeftAlone) {
  context_->UnfocusDocument(document_);
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL));
  EXPECT_EQ(paste_calls_, 0);
}

TEST_F(PasteTriggerTest, OnlyTheRightModifierCounts) {
  Focus("draft-input");
  EXPECT_TRUE(KeyV(0));
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL | ui::Input::KM_SHIFT)) << "Ctrl+Shift+V is someone else's";
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL | ui::Input::KM_ALT));
  EXPECT_TRUE(KeyV(ui::Input::KM_CTRL | ui::Input::KM_META));
  EXPECT_TRUE(KeyV(ui::Input::KM_META)) << "Cmd+V on a binding for Ctrl";
  EXPECT_EQ(paste_calls_, 0);
  EXPECT_FALSE(KeyV(ui::Input::KM_CTRL | ui::Input::KM_CAPSLOCK)) << "caps lock is not a modifier of the paste";
  EXPECT_EQ(paste_calls_, 1);
}

TEST_F(PasteTriggerTest, TheKeyBindingRunsBeforeTheTextFieldNotAfter) {
  Focus("draft-input");
  InputCoordinator input;
  input.Register(PasteKeyBinding(ports_, ui::Input::KM_CTRL));
  EXPECT_TRUE(input.ProcessKeyDown(context_, ui::Input::KI_V, ui::Input::KM_CTRL, /*priority_phase=*/false));
  EXPECT_EQ(paste_calls_, 0);
}

TEST_F(PasteTriggerTest, OtherKeysAreNotTheirs) {
  Focus("draft-input");
  InputCoordinator input;
  input.Register(PasteKeyBinding(ports_, ui::Input::KM_CTRL));
  EXPECT_TRUE(input.ProcessKeyDown(context_, ui::Input::KI_C, ui::Input::KM_CTRL, true));
  EXPECT_TRUE(input.ProcessKeyDown(context_, ui::Input::KI_RETURN, 0, true));
  EXPECT_EQ(paste_calls_, 0);
}

TEST_F(PasteTriggerTest, ThisSystemsPasteModifierIsCmdOnAMacAndCtrlElsewhere) {
  const bool mac = std::strcmp(Platform::OsName(), "macos") == 0;
  EXPECT_EQ(PasteKeyModifier(), mac ? ui::Input::KM_META : ui::Input::KM_CTRL);
}

TEST_F(PasteTriggerTest, TheMenuInterceptorActsOnlyOnTheChatBox) {
  const auto interceptor = PasteMenuInterceptor(ports_);
  EXPECT_TRUE(interceptor(document_->GetElementById("draft-input")));
  EXPECT_EQ(paste_calls_, 1);
  for (const char* id : {"ask-ai-input", "search-input", "ps-text"}) {
    EXPECT_FALSE(interceptor(document_->GetElementById(id))) << id;
  }
  EXPECT_FALSE(interceptor(document_->QuerySelector(".field")));
  EXPECT_FALSE(interceptor(nullptr));
  EXPECT_EQ(paste_calls_, 1) << "no other field reached the paste";
  chat_context_ = false;
  EXPECT_FALSE(interceptor(document_->GetElementById("draft-input")));
  EXPECT_EQ(paste_calls_, 1);
  EXPECT_EQ(clipboard_touches_, 1) << "only the one paste that ran looked at the clipboard (one types call)";
}

TEST_F(PasteTriggerTest, TheMenuInterceptorAnswersFalseWhenThePasteDeclines) {
  paste_result_ = false;
  EXPECT_FALSE(PasteMenuInterceptor(ports_)(document_->GetElementById("draft-input")));
  EXPECT_EQ(paste_calls_, 1);
}

// ---- ChatController's wiring, read from its source ---------------------------------------------------------------------

std::string ReadAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

std::string Body(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) {
    return {};
  }
  const size_t next = source.find("\n}\n", start);
  return source.substr(start, next == std::string::npos ? std::string::npos : next - start);
}

TEST(PasteWiringTest, TheTriggersAreInstalledOnAComputerOnlyAndShareOneFunction) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  const std::string install = Body(source, "void ChatController::InstallPasteTriggers()");
  ASSERT_FALSE(install.empty());
  EXPECT_NE(install.find("Platform::IsMobile()"), std::string::npos);
  EXPECT_LT(install.find("Platform::IsMobile()"), install.find("PasteKeyBinding"));
  EXPECT_NE(install.find("PasteMenuInterceptor"), std::string::npos);
  EXPECT_NE(install.find("TryPasteAttachment()"), std::string::npos);
  EXPECT_NE(install.find("InAiComposerContext()"), std::string::npos);
  EXPECT_NE(install.find("chat_.editing_message"), std::string::npos);
  EXPECT_NE(source.find("  InstallPasteTriggers();"), std::string::npos);
  EXPECT_NE(Body(source, "void ChatController::Shutdown()").find("SetPasteInterceptor({})"), std::string::npos)
      << "the interceptor holds the controller: cleared at shutdown";
}

TEST(PasteWiringTest, ThePasteIsReadOnTheUiThreadAndWrittenOnAWorker) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  const std::string paste = Body(source, "bool ChatController::TryPasteAttachment()");
  ASSERT_FALSE(paste.empty());
  EXPECT_NE(paste.find("ReadClipboardImage("), std::string::npos);
  EXPECT_NE(paste.find("max_file_bytes"), std::string::npos) << "the size limit is the existing one";
  EXPECT_NE(paste.find("PostWorkerNormal"), std::string::npos);
  EXPECT_NE(paste.find("WritePastedFile"), std::string::npos);
  EXPECT_NE(paste.find("AttachSource::Paste"), std::string::npos);
}

TEST(PasteWiringTest, EveryOwnedFileExitIsWiredToTheFolderBoundRelease) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  const std::string intake = Body(source, "AttachIntakePorts ChatController::MakeAttachIntakePorts()");
  EXPECT_NE(intake.find("ports.release_owned = [dir = PastedDir()]"), std::string::npos);
  EXPECT_NE(intake.find("ReleasePastedFile(path, dir)"), std::string::npos);
  const std::string queue = Body(source, "PictureSendPorts ChatController::MakePictureSendPorts(");
  EXPECT_NE(queue.find("ports.release_owned = [dir = PastedDir()]"), std::string::npos);
  EXPECT_NE(queue.find("ReleasePastedFile(path, PastedDir())"), std::string::npos)
      << "an item that ends after its queue is gone";
  // The user's own files keep going through ReleasePickedFile only; it is not widened.
  const std::string dialog =
      ReadAll(std::string(PP_SOURCE_DIR) + "/src/foundation/platform/NativeFileDialog_Default.cpp");
  EXPECT_EQ(dialog.find("pasted"), std::string::npos);
}

TEST(PasteWiringTest, ThePasteFlagIsReadFromThePasteAndTheNameReachesTheQueue) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  const std::string state = Body(source, "AttachState ChatController::CurrentAttachState() const");
  EXPECT_NE(state.find("paste_attach_->InFlight()"), std::string::npos);
  const std::string items = Body(source, "bool ChatController::OpenAttachItems(");
  EXPECT_NE(items.find("std::move(item.display_name)"), std::string::npos);
}

TEST(PasteWiringTest, TheStartUpSweepRunsOnceAfterTheFirstUnlockAndOnlyEmptiesThePastedFolder) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  const std::string wire = Body(source, "void ChatController::WireMessagingBindings()");
  ASSERT_FALSE(wire.empty());
  EXPECT_NE(wire.find("SweepPastedFilesOnce();"), std::string::npos);
  EXPECT_GT(wire.find("SweepPastedFilesOnce();"), wire.find("messaging_ready_ = true;"));
  const std::string sweep = Body(source, "void ChatController::SweepPastedFilesOnce()");
  EXPECT_NE(sweep.find("pasted_swept_"), std::string::npos);
  EXPECT_NE(sweep.find("SweepPastedDir(PastedDir())"), std::string::npos);
  const std::string dir = Body(source, "std::filesystem::path ChatController::PastedDir() const");
  EXPECT_NE(dir.find("AppPaths::CacheDir()"), std::string::npos);
  EXPECT_NE(dir.find("\"pasted\""), std::string::npos);
}

} // namespace
} // namespace pbr
