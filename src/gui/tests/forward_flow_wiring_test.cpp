// ChatController cannot be built in a unit test, so what it must do for a forward (DEV/PLAN-message-forward-text.md,
// step 4) is read from its source, as ChatPictureWiringTest does: deleting one of these calls fails here. What the flow
// does when it is called is tested in forward_flow_test.cpp.
#include <gtest/gtest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  std::string folded;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
      continue;
    }
    folded += text[i];
  }
  return folded;
}

std::string Body(const std::string& source, const std::string& signature) {
  const size_t at = source.find("\n" + signature);
  EXPECT_NE(at, std::string::npos) << signature;
  if (at == std::string::npos) {
    return {};
  }
  const size_t close = source.find("\n}\n", at);
  return source.substr(at + 1, close + 2 - (at + 1));
}

const std::string& Controller() {
  static const std::string source = ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
  return source;
}

TEST(ForwardFlowWiringTest, TheLockStopsAForwardBeforeTheViewCacheIsWiped) {
  const std::string update = Body(Controller(), "void ChatController::Update(");
  const size_t locked = update.find("if (!vault_locked_seen_) {");
  const size_t stop = update.find("forward_flow_->Stop()", locked);
  const size_t drop = update.find("picture_textures_.DropAllForLock", locked);
  ASSERT_NE(locked, std::string::npos);
  ASSERT_NE(stop, std::string::npos) << "the lock no longer stops a forward and closes its picker";
  EXPECT_LT(stop, drop);
}

TEST(ForwardFlowWiringTest, SigningOutStopsAForwardBeforeThePresenterForgetsTheProfile) {
  const std::string& source = Controller();
  const size_t reset = source.find("presenter_.ResetForNewProfile();", source.find("messaging_ready_ = false;"));
  ASSERT_NE(reset, std::string::npos);
  const size_t stop = source.rfind("forward_flow_->Stop();", reset);
  ASSERT_NE(stop, std::string::npos) << "the teardown no longer stops a forward";
  EXPECT_LT(source.rfind("messaging_ready_ = false;", reset), stop) << "inside the teardown, ahead of the reset";
  EXPECT_LT(reset - stop, 300u) << "right before the presenter forgets the profile";
}

TEST(ForwardFlowWiringTest, AnotherChatOrAGoneChatEndsTheChoosingButNotAForwardBeingSent) {
  const std::string select = Body(Controller(), "void ChatController::OnSelectThread(");
  EXPECT_NE(select.find("forward_flow_->Leave()"), std::string::npos);
  EXPECT_EQ(select.find("forward_flow_->Stop()"), std::string::npos)
      << "a send going on is not stopped by a chat switch";
  EXPECT_NE(select.find("forward_flow_->OnChatEntered()"), std::string::npos);
  EXPECT_LT(select.find("forward_flow_->Leave()"), select.find("presenter_.SelectThread(thread_id)"));
  const std::string forget = Body(Controller(), "void ChatController::ForgetComposerExtras(");
  EXPECT_NE(forget.find("forward_flow_->OnThreadGone(key)"), std::string::npos);
}

TEST(ForwardFlowWiringTest, SelectingAChatAlwaysSyncsTheForwardBarsAndTheTeardownStopsAForward) {
  const std::string select = Body(Controller(), "void ChatController::OnSelectThread(");
  const size_t leave = select.find("forward_flow_->Leave()");
  const size_t sync = select.find("OnForwardChanged();", leave);
  ASSERT_NE(sync, std::string::npos) << "a chat opening must re-sync the bar, whatever the presenter forgot before";
  EXPECT_LT(sync, select.find("presenter_.SelectThread(thread_id)"));
  const std::string torn = Body(Controller(), "void ChatController::OnMessagingTornDownForRegistration(");
  EXPECT_NE(torn.find("forward_flow_->Stop()"), std::string::npos)
      << "a forward still sending must not call into a torn-down messaging";
}

TEST(ForwardFlowWiringTest, StartingToChooseStopsAPlayingVoiceMessageAndARefusedPickerAnswersCancel) {
  const std::string begin = Body(Controller(), "void ChatController::BeginForward(");
  EXPECT_NE(begin.find("voice_playback_->Stop()"), std::string::npos);
  const size_t ports = Controller().find("forward_ports.open_picker =");
  ASSERT_NE(ports, std::string::npos);
  const std::string open = Controller().substr(ports, 700);
  EXPECT_NE(open.find("cancel()"), std::string::npos) << "no picker to open: the flow is told, not left waiting";
}

TEST(ForwardFlowWiringTest, TheRowsDecideForwardableFromTheMessageTheyWereBuiltFromNotAStoreRead) {
  const std::string decorate = Body(Controller(), "void ChatController::InstallRowDecorator(");
  EXPECT_EQ(decorate.find("presenter_.CanForward("), std::string::npos) << "one store read per row per redraw";
  EXPECT_NE(decorate.find("row.forwardable"), std::string::npos);
  const std::string presenter = ReadFile(std::string(PP_SOURCE_DIR) + "/src/feature/presenters/ChatPresenter.cpp");
  const std::string can = Body(presenter, "bool ChatPresenter::CanForward(");
  EXPECT_EQ(can.find("FacadeForwardPorts("), std::string::npos);
  const std::string now = Body(presenter, "const ForwardPorts& ChatPresenter::ForwardPortsNow(");
  EXPECT_NE(now.find("facade_ports_"), std::string::npos) << "the facade's ports are built once";
}

TEST(ForwardFlowWiringTest, TheRowsGetTheirFlagsFromThePresenterAndTheEventsGoToTheFlow) {
  const std::string decorate = Body(Controller(), "void ChatController::InstallRowDecorator(");
  EXPECT_NE(decorate.find("row.forward_ok = choosing && !IsPendingUploadRowId(id) && row.forwardable"),
            std::string::npos);
  EXPECT_NE(decorate.find("row.forward_ticked = choosing && forward_flow_->Ticked(id)"), std::string::npos);
  for (const char* event : {"\"forward_tap\"", "\"forward_cancel\"", "\"forward_next\""}) {
    EXPECT_NE(Controller().find(std::string("ctor.BindEventCallback(") + event), std::string::npos) << event;
  }
  EXPECT_NE(Controller().find("forward_flow_->Tap(message_id)"), std::string::npos);
  EXPECT_NE(Controller().find("forward_flow_->Cancel()"), std::string::npos);
  EXPECT_NE(Controller().find("forward_flow_->Next()"), std::string::npos);
}

TEST(ForwardFlowWiringTest, ThePickerAndThePillGoThroughTheAppsPorts) {
  const std::string ctor = Body(Controller(), "ChatController::ChatController()");
  EXPECT_NE(ctor.find("people_picker_notify_.open_choose_targets("), std::string::npos);
  EXPECT_NE(ctor.find("people_picker_notify_.close_choose_targets()"), std::string::npos);
  EXPECT_NE(ctor.find("forward_ports.pill = [this](const std::string& message) { ShowToast(message); }"),
            std::string::npos)
      << "results are the bottom white pill";
  const std::string app = ReadFile(std::string(PP_SOURCE_DIR) + "/src/app/Application.cpp");
  EXPECT_NE(app.find("chat_people_picker_notify.open_choose_targets"), std::string::npos);
  EXPECT_NE(app.find("people_picker_->OpenForResult("), std::string::npos);
  EXPECT_NE(app.find("people_picker_->CloseChooseTargets()"), std::string::npos);
}


// ---- Pictures, videos, files and voice messages (DEV/PLAN-message-forward-files-v3.md, step 3) --------------------

TEST(ForwardFlowWiringTest, TheProgressPageIsOpenedDrawnAndClosedThroughTheFlowsPorts) {
  const std::string ctor = Body(Controller(), "ChatController::ChatController()");
  for (const char* port : {"forward_ports.open_progress = [this]() { forward_progress_.Open(); }",
                           "forward_ports.close_progress = [this]() { forward_progress_.Close(); }",
                           "forward_ports.show_progress =", "forward_ports.keep_awake =", "forward_ports.now_ms =",
                           "forward_ports.preview = MakeForwardPreviewPorts()", "forward_ports.preview_done ="}) {
    EXPECT_NE(ctor.find(port), std::string::npos) << port;
  }
  EXPECT_NE(ctor.find("ScreenAwakeHolds::Shared().Set(ScreenAwakeHolds::Holder::Forward, on"), std::string::npos)
      << "the screen is held through the switch the call shares";
  for (const char* handler : {"forward_flow_->OnProgressCancel()", "forward_flow_->OnProgressClose()",
                              "forward_flow_->OnProgressBack()", "forward_flow_->OnProgressLayerClosed()"}) {
    EXPECT_NE(ctor.find(handler), std::string::npos) << handler;
  }
  EXPECT_NE(ctor.find("presenter_.SetForwardAttachmentBusy([this]() { return AttachmentBusy(); })"), std::string::npos)
      << "AttachmentBusy() reaches the presenter";
  EXPECT_NE(ctor.find("presenter_.SetForwardCallActive("), std::string::npos);
  EXPECT_NE(Controller().find("forward_progress_.RegisterModel(context)"), std::string::npos);
  const std::string header = ReadFile(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.h");
  EXPECT_NE(header.find("forward_progress_.BindFlowCoordinator(ports)"), std::string::npos);
  EXPECT_NE(Controller().find("forward_progress_.BindShellNavigation(shell_navigation_)"), std::string::npos);
}

TEST(ForwardFlowWiringTest, OnlyTheRealBackgroundStopsAForwardAndLosingFocusDoesNot) {
  const std::string setup = Body(Controller(), "bool ChatController::Setup(");
  const size_t listener = setup.find("PlatformLifecycleEvents::AddEnteredBackgroundListener");
  ASSERT_NE(listener, std::string::npos);
  EXPECT_NE(setup.find("forward_flow_->OnAppEnteredBackground()", listener), std::string::npos);
  EXPECT_NE(setup.rfind("if (Platform::IsMobile()) {", listener), std::string::npos)
      << "a desktop window losing focus or being minimised never stops it";
  // The focus-loss listener (iOS willResignActive: Control Center, a banner) is the one that already existed, and
  // does not touch the forward.
  const std::string pause = Body(Controller(), "void ChatController::OnApplicationPause()");
  EXPECT_EQ(pause.find("forward"), std::string::npos) << "a banner or Control Center must not stop a forward";
  EXPECT_NE(Body(Controller(), "void ChatController::Shutdown()").find("PlatformLifecycleEvents::RemoveListener"),
            std::string::npos);
}

TEST(ForwardFlowWiringTest, TheProgressPageGoesWhenACallStartsAndTheNumbersAreDrawnEveryTick) {
  const std::string update = Body(Controller(), "void ChatController::Update(");
  EXPECT_NE(update.find("if (forward_flow_->ProgressOpen() && ChromeSnapshot().call_in_progress) {"), std::string::npos);
  EXPECT_NE(update.find("forward_flow_->OnCallStarted()"), std::string::npos);
  EXPECT_NE(update.find("forward_flow_->Tick("), std::string::npos);
}

TEST(ForwardFlowWiringTest, ThePreviewTakesPicturesFromTheBubbleThenFromMemoryAndNeverDownloads) {
  const std::string ports = Body(Controller(), "ForwardPreviewPorts ChatController::MakeForwardPreviewPorts()");
  EXPECT_NE(ports.find("PictureSources({row})"), std::string::npos) << "the bubble's picture first";
  EXPECT_NE(ports.find("AttachmentPosterPath("), std::string::npos) << "a video's poster";
  EXPECT_NE(ports.find("StoredAttachmentPlaintext("), std::string::npos) << "the stored bytes, only for the shown few";
  EXPECT_NE(ports.find("MakeImageThumbnailJpeg("), std::string::npos);
  EXPECT_EQ(ports.find("Download"), std::string::npos) << "nothing is ever downloaded for a preview";
  EXPECT_NE(Controller().find("std::filesystem::remove_all(forward_preview_dir_"), std::string::npos)
      << "the thumbnails go when the page does";
}

} // namespace
} // namespace pbr
