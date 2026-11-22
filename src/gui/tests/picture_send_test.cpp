// The picture preview page (DEV/PLAN-picture-send-preview.md). Two kinds of check:
//  - the controller on its own, with a real PictureSendQueue behind it and every port faked: Back, Send, the lock while
//    the page is open, the worker that must stop before its folder goes, the textures given back on a swipe;
//  - the real page (views/picture_send.rml expanded by the real ViewCatalog, the real stylesheets) on a phone's width
//    with one and with nine pictures: nothing runs past the screen, the field and Send are inside it, the count leaves
//    out what is too large, the size shows only with "original" on, removing updates the count.
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/PictureSendController.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/platform/IAssetLocator.h"
#include "lib/ui/ViewCatalog.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/style/ComputedValues.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr uint64_t kMb = 1024 * 1024;

/** The first page's folder as the controller builds it (the native separator: a backslash before "1" on Windows). */
std::string PageDir() {
  return (std::filesystem::path("/nonexistent/picked/preview") / "1").string();
}

/** Everything the controller and its queue ask of the app, recorded. */
struct Fakes {
  std::vector<std::string> released;      // picked copies the queue let go
  std::vector<std::string> removed_files; // preview files the queue deleted
  std::vector<std::string> removed_paths; // what the controller deleted (files and the page folder)
  std::vector<std::pair<std::string, std::string>> put_back;
  std::vector<std::pair<std::string, std::string>> texts;
  std::vector<std::pair<std::string, bool>> sent_items; // path, original
  std::vector<std::vector<std::string>> textures;
  std::vector<std::string> played;
  std::vector<std::string> made; // files a preview was made for
  uint64_t made_limit = 0;       // the byte limit the last preview was given
  int left = 0;
  int queue_changed = 0;
  int first_preview = 0;
  int pushed = 0;
  int closed = 0;
  int ended = 0;
  /** B236: whether the host lets Send go (a held chat: no), and how often it was asked. */
  bool may_send = true;
  int send_asks = 0;
  int next_layer = 7;
  std::function<void()> on_cancel;
  std::vector<PictureSendReport> reports;
  /** Background work and answers wait here until the test runs them (a worker "still running"). */
  std::vector<std::function<void()>> worker;
  std::vector<std::function<void()>> ui;
  bool inline_work = true;
};

PictureSendPorts QueuePorts(Fakes& fakes) {
  PictureSendPorts ports;
  ports.send_item = [&fakes](const PictureSendItem& item, const bool original) {
    fakes.sent_items.emplace_back(item.path, original);
  };
  ports.send_text = [&fakes](const std::string& thread, const std::string& text) {
    fakes.texts.emplace_back(thread, text);
    return true;
  };
  ports.put_back_text = [&fakes](const std::string& thread, const std::string& text) {
    fakes.put_back.emplace_back(thread, text);
  };
  ports.release = [&fakes](const std::string& path) { fakes.released.push_back(path); };
  ports.remove_file = [&fakes](const std::string& path) { fakes.removed_files.push_back(path); };
  ports.now_ms = []() { return int64_t{0}; };
  ports.schedule_tick = [](int64_t) {};
  ports.offline = []() { return false; };
  ports.done = [&fakes](const PictureSendReport& report) { fakes.reports.push_back(report); };
  return ports;
}

class PictureSendControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    queue_ = std::make_unique<PictureSendQueue>("chat-origin", AttachmentLimits{}, QueuePorts(fakes_));
    ShellNavigationPorts navigation;
    navigation.push_layer = [this](const PaneSpec& spec) {
      ++fakes_.pushed;
      EXPECT_EQ(spec.key, "picture_send");
      EXPECT_TRUE(spec.full_screen);
      return fakes_.next_layer;
    };
    navigation.close_layer = [this](int) { ++fakes_.closed; };
    controller_.BindShellNavigation(std::move(navigation));
    FlowCoordinatorPorts flow;
    flow.begin_modal = [this](int, std::function<bool()>, std::function<void()> on_cancel) {
      fakes_.on_cancel = std::move(on_cancel);
    };
    flow.end_modal = [this]() { ++fakes_.ended; };
    controller_.BindFlowCoordinator(std::move(flow));
    PictureSendHost host;
    host.queue = [this]() { return queue_.get(); };
    host.preview_root = []() { return std::string("/nonexistent/picked/preview"); };
    host.make_preview = [this](const std::string& path, PictureSendKind, const std::string& dir,
                               const uint64_t max_image_bytes, const std::function<bool()>& cancelled) {
      fakes_.made.push_back(path);
      fakes_.made_limit = max_image_bytes;
      PictureSendPreviewFiles files;
      if (!cancelled()) {
        files.preview_path = dir + "/" + path + ".big.jpg";
        files.thumb_path = dir + "/" + path + ".small.jpg";
        files.width = 1280;
        files.height = 960;
      }
      return files;
    };
    host.release_textures = [this](std::vector<std::string> srcs) { fakes_.textures.push_back(std::move(srcs)); };
    host.queue_changed = [this]() { ++fakes_.queue_changed; };
    host.left = [this]() { ++fakes_.left; };
    host.first_preview_shown = [this]() { ++fakes_.first_preview; };
    host.may_send = [this]() {
      ++fakes_.send_asks;
      return fakes_.may_send;
    };
    host.send_label = [](size_t n) { return n <= 1 ? std::string("Send") : "Send (" + std::to_string(n) + ")"; };
    host.size_label = [](uint64_t bytes) { return FormatPickedSize(bytes) + " total"; };
    host.post_worker = [this](std::function<void()> work) {
      if (fakes_.inline_work) {
        work();
      } else {
        fakes_.worker.push_back(std::move(work));
      }
    };
    host.post_ui = [this](std::function<void()> work) {
      if (fakes_.inline_work) {
        work();
      } else {
        fakes_.ui.push_back(std::move(work));
      }
    };
    host.remove_path = [this](const std::string& path) { fakes_.removed_paths.push_back(path); };
    host.play_video = [this](const std::string& path) {
      fakes_.played.push_back(path);
      return true;
    };
    controller_.SetHost(std::move(host));
  }

  /** A picked copy arrives: added to the queue, shown on the page. */
  uint64_t Arrive(const std::string& path, const uint64_t bytes = kMb,
                  const PictureSendKind kind = PictureSendKind::Picture) {
    const auto id = queue_->Add(path, kind, bytes, 1000 * 1000);
    EXPECT_TRUE(id.has_value());
    EXPECT_TRUE(controller_.OnItemAdded(*id));
    return *id;
  }
  /** The worker has finished and answers (background work held back with inline_work = false). */
  void RunWorker() {
    ASSERT_FALSE(fakes_.worker.empty());
    auto work = std::move(fakes_.worker.front());
    fakes_.worker.erase(fakes_.worker.begin());
    work();
  }
  void RunUi() {
    ASSERT_FALSE(fakes_.ui.empty());
    auto reply = std::move(fakes_.ui.front());
    fakes_.ui.erase(fakes_.ui.begin());
    reply();
  }
  bool Removed(const std::string& path) const {
    return std::find(fakes_.removed_paths.begin(), fakes_.removed_paths.end(), path) != fakes_.removed_paths.end();
  }

  Fakes fakes_;
  std::unique_ptr<PictureSendQueue> queue_;
  PictureSendController controller_;
};

TEST_F(PictureSendControllerTest, ThePageOpensOnTheFirstPictureAndHoldsOneLayer) {
  Arrive("a.jpg");
  EXPECT_TRUE(controller_.IsOpen());
  Arrive("b.jpg");
  EXPECT_EQ(fakes_.pushed, 1) << "one page for the whole pick; later pictures only join it";
  ASSERT_EQ(controller_.Cells().size(), 2U);
  EXPECT_EQ(fakes_.first_preview, 1) << "the first picture shown is timed once";
}

TEST_F(PictureSendControllerTest, BackSendsNothingReleasesTheFilesAndPutsTheTextInTheOriginChat) {
  Arrive("a.jpg");
  Arrive("b.jpg");
  controller_.SetTextForTest("hello there");
  controller_.OnBack();
  EXPECT_FALSE(controller_.IsOpen());
  EXPECT_EQ(fakes_.ended, 1) << "its own close is not also a cancel";
  EXPECT_EQ(fakes_.closed, 1);
  EXPECT_TRUE(fakes_.sent_items.empty());
  EXPECT_TRUE(fakes_.texts.empty());
  ASSERT_EQ(fakes_.put_back.size(), 1U);
  EXPECT_EQ(fakes_.put_back[0].first, "chat-origin") << "the chat the pictures were picked in";
  EXPECT_EQ(fakes_.put_back[0].second, "hello there");
  EXPECT_EQ(fakes_.released, (std::vector<std::string>{"a.jpg", "b.jpg"}));
  EXPECT_TRUE(queue_->Ended());
  EXPECT_EQ(fakes_.left, 1);
  EXPECT_GE(fakes_.queue_changed, 1);
  EXPECT_TRUE(Removed(PageDir())) << "the page's folder goes";
}

TEST_F(PictureSendControllerTest, BackWithNoTextPutsNothingBack) {
  Arrive("a.jpg");
  controller_.OnBack();
  EXPECT_TRUE(fakes_.put_back.empty());
  EXPECT_EQ(fakes_.released, std::vector<std::string>{"a.jpg"});
}

TEST_F(PictureSendControllerTest, ALayerClosedByAnyoneIsBack) {
  Arrive("a.jpg");
  controller_.SetTextForTest("typed");
  ASSERT_TRUE(fakes_.on_cancel) << "the back key, a swipe and a tapped notification all arrive here";
  fakes_.on_cancel();
  EXPECT_FALSE(controller_.IsOpen());
  EXPECT_EQ(fakes_.closed, 0) << "the layer was already closed by whoever cancelled";
  ASSERT_EQ(fakes_.put_back.size(), 1U);
  EXPECT_EQ(fakes_.put_back[0].second, "typed");
  EXPECT_EQ(fakes_.released, std::vector<std::string>{"a.jpg"});
}

TEST_F(PictureSendControllerTest, TheOriginalCheckIsOffEachTimeThePageOpensAndOnlyATapSetsIt) {
  Arrive("a.jpg");
  EXPECT_FALSE(controller_.OriginalChecked()) << "off when the page opens";
  controller_.OnToggleOriginal();
  EXPECT_TRUE(controller_.OriginalChecked());
  EXPECT_TRUE(queue_->Original());
  controller_.OnToggleOriginal();
  EXPECT_FALSE(controller_.OriginalChecked());
  controller_.OnToggleOriginal();
  controller_.OnSend();
  ASSERT_FALSE(controller_.IsOpen());
  EXPECT_TRUE(fakes_.sent_items.at(0).second) << "that send went as original";
  // The next pick opens the page again with a new queue: off, whatever the last send was.
  queue_ = std::make_unique<PictureSendQueue>("chat-origin", AttachmentLimits{}, QueuePorts(fakes_));
  Arrive("b.jpg");
  EXPECT_TRUE(controller_.IsOpen());
  EXPECT_FALSE(controller_.OriginalChecked());
  EXPECT_FALSE(queue_->Original());
  // Leaving with it checked and opening again: off too.
  controller_.OnToggleOriginal();
  controller_.OnBack();
  queue_ = std::make_unique<PictureSendQueue>("chat-origin", AttachmentLimits{}, QueuePorts(fakes_));
  Arrive("c.jpg");
  EXPECT_FALSE(controller_.OriginalChecked());
}

TEST_F(PictureSendControllerTest, SendStartsTheQueueWithTheTextAndTheOriginalChoiceAndClosesThePage) {
  Arrive("a.jpg");
  Arrive("b.jpg");
  controller_.OnToggleOriginal();
  controller_.SetTextForTest("two pictures");
  controller_.OnSend();
  EXPECT_FALSE(controller_.IsOpen());
  EXPECT_EQ(fakes_.closed, 1);
  EXPECT_EQ(fakes_.ended, 1);
  EXPECT_TRUE(queue_->Started());
  ASSERT_EQ(fakes_.sent_items.size(), 1U) << "one at a time";
  EXPECT_EQ(fakes_.sent_items[0], (std::pair<std::string, bool>{"a.jpg", true})) << "in order, as they are";
  EXPECT_EQ(queue_->Text(), "two pictures");
  EXPECT_TRUE(fakes_.put_back.empty());
  // The text follows the last picture.
  queue_->OnItemFinished(1, AttachmentSendOutcome::Sent);
  EXPECT_TRUE(fakes_.texts.empty());
  queue_->OnItemFinished(2, AttachmentSendOutcome::Sent);
  ASSERT_EQ(fakes_.texts.size(), 1U);
  EXPECT_EQ(fakes_.texts[0], (std::pair<std::string, std::string>{"chat-origin", "two pictures"}));
  EXPECT_TRUE(Removed(PageDir()));
}

// B236: in a held chat the page's Send queues nothing and the page stays open with its pictures and text (the host shows
// the pill each time); once the chat may send again, the same Send goes.
TEST_F(PictureSendControllerTest, SendInAHeldChatQueuesNothingAndThePageStays) {
  Arrive("a.jpg");
  controller_.SetTextForTest("hi");
  fakes_.may_send = false;
  controller_.OnSend();
  controller_.OnSend();
  EXPECT_EQ(fakes_.send_asks, 2) << "asked on every tap";
  EXPECT_TRUE(controller_.IsOpen());
  EXPECT_FALSE(queue_->Started());
  EXPECT_TRUE(fakes_.sent_items.empty());
  EXPECT_EQ(fakes_.closed, 0);
  fakes_.may_send = true;
  controller_.OnSend();
  EXPECT_TRUE(queue_->Started());
  EXPECT_FALSE(controller_.IsOpen());
}

TEST_F(PictureSendControllerTest, SendDoesNothingWhenNothingCanBeSent) {
  Arrive("big.mov", 80 * kMb, PictureSendKind::Video); // over the video limit
  EXPECT_FALSE(controller_.CanSend());
  controller_.OnSend();
  EXPECT_TRUE(controller_.IsOpen());
  EXPECT_FALSE(queue_->Started());
}

TEST_F(PictureSendControllerTest, TheLockWhileOpenClosesThePageAndSendsNothing) {
  Arrive("a.jpg");
  controller_.SetTextForTest("secret");
  controller_.CloseForExit(); // what the chat screen does first on a lock, a sign-out or a deleted chat
  EXPECT_FALSE(controller_.IsOpen());
  EXPECT_EQ(fakes_.closed, 1);
  EXPECT_EQ(fakes_.ended, 1);
  EXPECT_TRUE(fakes_.put_back.empty()) << "closing the page is not a Back: the text is not revived";
  EXPECT_TRUE(fakes_.sent_items.empty());
  queue_->Locked(); // then the queue's own exit
  EXPECT_TRUE(fakes_.put_back.empty()) << "the text is dropped, not put back";
  EXPECT_TRUE(fakes_.texts.empty());
  EXPECT_EQ(fakes_.released, std::vector<std::string>{"a.jpg"});
  EXPECT_TRUE(Removed(PageDir()));
  controller_.CloseForExit();
  EXPECT_EQ(fakes_.closed, 1) << "a second call closes nothing more";
}

TEST_F(PictureSendControllerTest, CloseForExitActsOnceAndAnIdleControllerIsNotDirtiedEveryFrame) {
  const int never_opened = controller_.DirtyCountForTest();
  for (int i = 0; i < 5; ++i) {
    controller_.CloseForExit(); // every frame while locked, nothing open
  }
  EXPECT_EQ(controller_.DirtyCountForTest(), never_opened);
  Arrive("a.jpg");
  controller_.CloseForExit();
  const int after_close = controller_.DirtyCountForTest();
  for (int i = 0; i < 5; ++i) {
    controller_.CloseForExit();
  }
  EXPECT_EQ(controller_.DirtyCountForTest(), after_close) << "the first call cleared everything; later ones do nothing";
  EXPECT_EQ(fakes_.closed, 1);
}

TEST_F(PictureSendControllerTest, EachTextureIsGivenBackOnceAcrossSwipeRemoveAndStop) {
  Arrive("a.jpg");
  Arrive("b.jpg");
  Arrive("c.jpg");
  controller_.OnPress(300);
  controller_.OnRelease(100); // a -> b: a's large picture goes back
  controller_.OnRemove();     // b removed (its large picture and thumbnail), c is shown
  controller_.OnBack();       // the rest goes back at the end
  std::map<std::string, int> seen;
  for (const auto& batch : fakes_.textures) {
    for (const std::string& src : batch) {
      ++seen[src];
    }
  }
  for (const auto& entry : seen) {
    EXPECT_EQ(entry.second, 1) << entry.first << " was released more than once";
  }
  const std::string dir = PageDir() + "/";
  for (const char* name :
       {"a.jpg.big.jpg", "b.jpg.big.jpg", "b.jpg.small.jpg", "c.jpg.big.jpg", "a.jpg.small.jpg", "c.jpg.small.jpg"}) {
    EXPECT_EQ(seen.count(dir + name), 1U) << name << " was never given back";
  }
}

TEST_F(PictureSendControllerTest, AReleaseWithoutAPressOnTheStageIsNotASwipe) {
  Arrive("a.jpg");
  Arrive("b.jpg");
  controller_.OnRelease(-500); // a drag that began outside the stage
  EXPECT_EQ(controller_.CurrentIndex(), 0);
  controller_.OnPress(300);
  controller_.OnRelease(100);
  EXPECT_EQ(controller_.CurrentIndex(), 1);
  controller_.OnRelease(900); // the same press cannot swipe twice
  EXPECT_EQ(controller_.CurrentIndex(), 1);
}

TEST_F(PictureSendControllerTest, AnItemOverTheLimitGetsNoPreviewAndOthersAreReadWithTheBatchLimit) {
  Arrive("big.mov", 80 * kMb, PictureSendKind::Video); // over the video limit
  Arrive("a.jpg");
  EXPECT_EQ(fakes_.made, std::vector<std::string>{"a.jpg"}) << "no decode for what cannot be sent";
  EXPECT_EQ(fakes_.made_limit, queue_->Limits().max_original_image_bytes);
  EXPECT_TRUE(controller_.Cells()[0].too_large);
  EXPECT_FALSE(controller_.Cells()[0].ready);
}

TEST_F(PictureSendControllerTest, ThePageFolderOnlyGoesAfterTheWorkerThatMayWriteIntoItHasStopped) {
  fakes_.inline_work = false;
  Arrive("a.jpg");
  ASSERT_EQ(fakes_.worker.size(), 1U) << "a preview is being made";
  controller_.OnBack();
  EXPECT_TRUE(controller_.WorkerRunning());
  EXPECT_FALSE(Removed(PageDir())) << "the worker may still write into it";
  RunWorker(); // it sees the stop flag before writing
  RunUi();     // and reports
  EXPECT_FALSE(controller_.WorkerRunning());
  EXPECT_TRUE(Removed(PageDir()));
}

TEST_F(PictureSendControllerTest, PreviewsAreMadeOneAtATimeAndABoundFileIsOneThatExists) {
  fakes_.inline_work = false;
  Arrive("a.jpg");
  Arrive("b.jpg");
  EXPECT_EQ(fakes_.worker.size(), 1U) << "the second waits for the first";
  EXPECT_EQ(controller_.CurrentSrc(), "") << "nothing is bound before its file is written";
  RunWorker();
  RunUi();
  EXPECT_EQ(controller_.CurrentSrc(), PageDir() + "/a.jpg.big.jpg");
  EXPECT_EQ(fakes_.worker.size(), 1U) << "now the second";
  EXPECT_EQ(fakes_.first_preview, 1);
}

TEST_F(PictureSendControllerTest, AFileMadeForAPictureRemovedMeanwhileIsDeleted) {
  fakes_.inline_work = false;
  Arrive("a.jpg");
  Arrive("b.jpg");
  controller_.OnRemove(); // the current one, a.jpg, while its preview is being made
  EXPECT_EQ(fakes_.released, std::vector<std::string>{"a.jpg"});
  RunWorker();
  RunUi();
  EXPECT_TRUE(Removed(PageDir() + "/a.jpg.big.jpg") || fakes_.worker.size() == 1U)
      << "a stopped worker wrote nothing, or what it wrote is deleted";
  EXPECT_EQ(controller_.Cells().size(), 1U);
}

TEST_F(PictureSendControllerTest, SwipingAwayGivesTheTextureBackAndASwipeNeedsAFingerMove) {
  Arrive("a.jpg");
  Arrive("b.jpg");
  const std::string first = PageDir() + "/a.jpg.big.jpg";
  EXPECT_EQ(controller_.CurrentSrc(), first);
  controller_.OnPress(300);
  controller_.OnRelease(290); // a tap, not a swipe
  EXPECT_EQ(controller_.CurrentIndex(), 0);
  controller_.OnPress(300);
  controller_.OnRelease(100); // left: the next one
  EXPECT_EQ(controller_.CurrentIndex(), 1);
  ASSERT_FALSE(fakes_.textures.empty());
  EXPECT_EQ(fakes_.textures.back(), std::vector<std::string>{first});
  controller_.OnPress(100);
  controller_.OnRelease(300); // right: back
  EXPECT_EQ(controller_.CurrentIndex(), 0);
  controller_.OnPress(100);
  controller_.OnRelease(300);
  EXPECT_EQ(controller_.CurrentIndex(), 0) << "no page before the first";
}

TEST_F(PictureSendControllerTest, RemovingTheLastPictureLeavesThePage) {
  Arrive("a.jpg");
  controller_.SetTextForTest("kept");
  controller_.OnRemove();
  EXPECT_FALSE(controller_.IsOpen());
  EXPECT_EQ(fakes_.released, std::vector<std::string>{"a.jpg"});
  ASSERT_EQ(fakes_.put_back.size(), 1U) << "leaving with an empty page is Back: the text returns";
  EXPECT_EQ(fakes_.put_back[0].second, "kept");
}

TEST_F(PictureSendControllerTest, WhileMoreAreComingAnEmptiedPageStaysAndClosesWhenNothingMoreComes) {
  Arrive("a.jpg");
  controller_.SetArriving(true);
  controller_.OnRemove();
  EXPECT_TRUE(controller_.IsOpen()) << "more pictures are still being copied";
  controller_.SetArriving(false);
  EXPECT_FALSE(controller_.IsOpen());
}

TEST_F(PictureSendControllerTest, PlayingAPickedVideoOpensThePlayerOnItsFileAndLeavesThePageAsItWas) {
  Arrive("a.jpg");
  Arrive("clip.mov", kMb, PictureSendKind::Video);
  controller_.SetTextForTest("typed");
  controller_.OnToggleOriginal();
  controller_.OnPlay();
  EXPECT_TRUE(fakes_.played.empty()) << "the current picture is a picture: nothing to play";
  controller_.OnSelect(1);
  controller_.OnPlay();
  EXPECT_EQ(fakes_.played, std::vector<std::string>{"clip.mov"}) << "the picked copy itself";
  EXPECT_TRUE(controller_.IsOpen());
  EXPECT_EQ(controller_.CurrentIndex(), 1);
  EXPECT_EQ(controller_.Text(), "typed");
  EXPECT_TRUE(queue_->Original());
  EXPECT_EQ(queue_->Items().size(), 2U);
  EXPECT_TRUE(fakes_.released.empty());
  // The page still leaves cleanly with the player up: Back releases the copy the player was given.
  controller_.OnBack();
  EXPECT_EQ(fakes_.released, (std::vector<std::string>{"a.jpg", "clip.mov"}));
}

TEST_F(PictureSendControllerTest, ThePickedSizeIsOneDecimalBelowTenMegabytes) {
  EXPECT_EQ(FormatPickedSize(7 * kMb), "7.0 MB");
  EXPECT_EQ(FormatPickedSize(kMb / 2), "0.5 MB");
  EXPECT_EQ(FormatPickedSize(48 * kMb), "48 MB");
}

// ---- the real page --------------------------------------------------------------------------------------------

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

std::string AssetPath(const std::string& relative) {
  return std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative;
}

class AssetsLocator : public IAssetLocator {
public:
  std::string Resolve(const std::string& relative) const override { return AssetPath(relative); }
};

/** Shown: neither it nor an ancestor is display:none (a hidden element keeps its last box), and it has a box. */
bool IsShown(ui::Element* element) {
  for (ui::Element* at = element; at != nullptr; at = at->GetParentNode()) {
    if (at->GetComputedValues().display() == ui::Style::Display::None) {
      return false;
    }
  }
  return element->GetBox().GetSize(ui::BoxArea::Border).x > 0.f;
}

struct Edges {
  float left, top, right, bottom;
};

Edges BorderEdges(ui::Element* element) {
  const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
  return {at.x, at.y, at.x + size.x, at.y + size.y};
}

class PictureSendPageTest : public PictureSendControllerTest, public ::testing::WithParamInterface<int> {
protected:
  void SetUp() override {
    PictureSendControllerTest::SetUp();
    ViewCatalog::ClearCache();
    IAssetLocator::SetInstance(nullptr);
    IAssetLocator::SetInstance(&locator_);
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ui::SetRenderInterface(&render_);
    ASSERT_TRUE(ui::Initialise());
    ASSERT_TRUE(ui::LoadFontFace(AssetPath("fonts/Inter-Regular.ttf"), /*fallback_face=*/true));
    context_ = ui::CreateContext("picture-send-page-test", {width(), kHeight});
    ASSERT_NE(context_, nullptr);
    ASSERT_TRUE(controller_.RegisterModel(context_));
    // The page as the shell lays it out: a full-screen layer with the status bar and the home indicator kept clear.
    const std::string body = LocalizationService::Instance().LocalizeText(ViewCatalog::LoadBody("picture_send"));
    ASSERT_FALSE(body.empty());
    ASSERT_EQ(body.find("{{include:"), std::string::npos);
    const std::string rml = "<rml><head><style>" + ReadAll(AssetPath("themes/components.rcss")) +
                            ReadAll(AssetPath("themes/colors-light.rcss")) +
                            "</style></head><body style=\"padding: 0; font-family: Inter; font-size: 15dp;\">"
                            "<div style=\"position: absolute; top: 0; left: 0; width: " +
                            std::to_string(width()) + "dp; height: " + std::to_string(kHeight) +
                            "dp; padding: 47dp 0 34dp 0; box-sizing: border-box;\">" + body + "</div></body></rml>";
    document_ = context_->LoadDocumentFromMemory(rml);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    Frames();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context_) {
      ui::RemoveContext("picture-send-page-test");
    }
    ui::Shutdown();
    ui::SetRenderInterface(nullptr);
    IAssetLocator::SetInstance(nullptr);
    ViewCatalog::ClearCache();
  }
  static constexpr int kHeight = 812;
  int width() const { return GetParam(); }
  void Frames() {
    for (int i = 0; i < 3; ++i) {
      controller_.Update();
      context_->Update();
    }
  }
  ui::Element* One(const char* selector) {
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    return found.empty() ? nullptr : found.front();
  }
  /** The elements of the page that are shown (a data-if / data-for template that is hidden has no box). */
  size_t Count(const char* selector) {
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    return static_cast<size_t>(std::count_if(found.begin(), found.end(), IsShown));
  }
  /** The width of what is shown; 0 for what is hidden. */
  static float Width(ui::Element* element) {
    return IsShown(element) ? element->GetBox().GetSize(ui::BoxArea::Border).x : 0.f;
  }
  std::string Text(const char* selector) {
    ui::Element* element = One(selector);
    return element ? std::string(element->GetInnerRML().c_str()) : "<none>";
  }
  void Tap(const char* selector, const size_t index = 0) {
    ui::ElementList found;
    document_->QuerySelectorAll(found, selector);
    std::vector<ui::Element*> shown;
    for (ui::Element* element : found) {
      if (IsShown(element)) {
        shown.push_back(element);
      }
    }
    ASSERT_LT(index, shown.size()) << selector;
    shown[index]->DispatchEvent(ui::EventId::Click, {});
    Frames();
  }
  void Fill(const int count) {
    for (int i = 0; i < count; ++i) {
      Arrive("p" + std::to_string(i) + ".jpg", 2 * kMb);
    }
    Frames();
  }
  /** Inside the screen, both ways. */
  void ExpectInside(const char* selector) {
    ui::Element* element = One(selector);
    ASSERT_NE(element, nullptr) << selector;
    const Edges edges = BorderEdges(element);
    EXPECT_GE(edges.left, 0.f) << selector;
    EXPECT_LE(edges.right, static_cast<float>(width()) + 0.5f) << selector;
    EXPECT_GE(edges.top, 0.f) << selector;
    EXPECT_LE(edges.bottom, static_cast<float>(kHeight) + 0.5f) << selector;
  }

  AssetsLocator locator_;
  NullRender render_;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
};

INSTANTIATE_TEST_SUITE_P(PhoneWidths, PictureSendPageTest, ::testing::Values(375, 393));

TEST_P(PictureSendPageTest, OneAndNinePicturesFitTheScreenWithTheFieldAndSendInsideIt) {
  for (const int count : {1, 9}) {
    SCOPED_TRACE("pictures: " + std::to_string(count));
    queue_ = std::make_unique<PictureSendQueue>("chat-origin", AttachmentLimits{}, QueuePorts(fakes_));
    Fill(count);
    EXPECT_LE(BorderEdges(One(".ps-send")).bottom, static_cast<float>(kHeight) - 34.f + 0.5f)
        << "above the home indicator";
    for (const char* selector :
         {".picture-send", ".ps-top", ".ps-stage", ".ps-bottom", ".ps-field", "#ps-text", ".ps-original", ".ps-send"}) {
      ExpectInside(selector);
    }
    EXPECT_EQ(Count(".ps-cell"), count > 1 ? static_cast<size_t>(count) : 0U) << "a strip only for several";
    if (count > 1) {
      ExpectInside(".ps-strip");
      ui::ElementList cells;
      document_->QuerySelectorAll(cells, ".ps-cell");
      for (ui::Element* cell : cells) {
        const ui::Vector2f size = cell->GetBox().GetSize(ui::BoxArea::Border);
        if (size.x > 0.f) {
          EXPECT_GE(size.x, 44.f) << "a thumbnail is a 44dp target";
          EXPECT_GE(size.y, 44.f);
        }
      }
    }
    // The page itself never scrolls sideways: the widest thing in it is no wider than the screen.
    EXPECT_LE(Width(One(".picture-send")), static_cast<float>(width()) + 0.5f);
    // Targets of at least 44dp: back, remove, the original row, Send.
    EXPECT_EQ(Count(".ps-badge"), count > 1 ? 1U : 0U) << "a remove badge only where there is a strip";
    EXPECT_GE(Width(One(".ps-top-btn")), 44.f);
    for (const char* selector : {".ps-top-btn", ".ps-original", ".ps-send", ".ps-field"}) {
      EXPECT_GE(One(selector)->GetBox().GetSize(ui::BoxArea::Border).y, 44.f) << selector;
    }
    // The picture keeps its proportions (1280 x 960 here) and fits the stage: not stretched, not left out.
    ui::Element* image = One(".ps-image");
    ASSERT_NE(image, nullptr);
    ASSERT_TRUE(IsShown(image)) << "a picture whose file exists is on the page";
    const ui::Vector2f image_size = image->GetBox().GetSize(ui::BoxArea::Border);
    const ui::Vector2f stage_size = One(".ps-stage")->GetBox().GetSize(ui::BoxArea::Border);
    EXPECT_NEAR(image_size.x / image_size.y, 4.f / 3.f, 0.03f);
    EXPECT_LE(image_size.x, stage_size.x + 0.5f);
    EXPECT_LE(image_size.y, stage_size.y + 0.5f);
    EXPECT_EQ(Text(".ps-title"), "1 / " + std::to_string(count));
    controller_.OnBack();
    Frames();
    EXPECT_FALSE(controller_.IsOpen());
    fakes_ = Fakes{};
    queue_ = std::make_unique<PictureSendQueue>("chat-origin", AttachmentLimits{}, QueuePorts(fakes_));
  }
}

// Boxes are measured in pixels and the stylesheet is in dp: on a 3x phone the fitted picture was 3 times too big and
// ran out of its stage (the simulator showed an empty stage, 2026-11-20).
TEST_P(PictureSendPageTest, ACollapsedStageSettlesInsteadOfRefittingEveryFrame) {
  Fill(1);
  ui::Element* stage = One("#ps-stage");
  ASSERT_NE(stage, nullptr);
  Frames();
  ASSERT_GT(stage->GetBox().GetSize(ui::BoxArea::Content).y, 1.f) << "laid out first";
  for (const char* property : {"height", "min-height", "max-height", "flex"}) {
    stage->SetProperty(property, std::string(property) == "flex" ? "0 0 0dp" : "0dp"); // the stage collapses
  }
  Frames();
  ASSERT_LE(stage->GetBox().GetSize(ui::BoxArea::Content).y, 1.f) << "the stage really collapsed";
  const int settled = controller_.DirtyCountForTest();
  Frames();
  Frames();
  EXPECT_EQ(controller_.DirtyCountForTest(), settled) << "a stage that stays collapsed is fitted once, not per frame";
}

TEST_P(PictureSendPageTest, TheFittedPictureStaysInsideItsStageOnADenseScreen) {
  context_->SetDensityIndependentPixelRatio(3.f);
  Frames();
  Fill(2);
  Frames();
  ui::Element* image = One(".ps-image");
  ASSERT_NE(image, nullptr);
  ASSERT_TRUE(IsShown(image));
  const ui::Vector2f image_size = image->GetBox().GetSize(ui::BoxArea::Border);
  const ui::Vector2f stage_size = One(".ps-stage")->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_LE(image_size.x, stage_size.x + 1.f);
  EXPECT_LE(image_size.y, stage_size.y + 1.f);
  EXPECT_NEAR(image_size.x / image_size.y, 4.f / 3.f, 0.03f);
}

TEST_P(PictureSendPageTest, AVideoHasAPlayButtonOfAtLeast44dpInTheMiddleAndAPictureHasNone) {
  Arrive("a.jpg");
  Arrive("clip.mov", kMb, PictureSendKind::Video);
  Frames();
  EXPECT_EQ(Count(".ps-play-btn"), 0U) << "a picture has nothing to play";
  Tap(".ps-cell", 1);
  ASSERT_EQ(Count(".ps-play-btn"), 1U);
  ui::Element* button = One(".ps-play-btn");
  const Edges edges = BorderEdges(button);
  EXPECT_GE(edges.right - edges.left, 44.f);
  EXPECT_GE(edges.bottom - edges.top, 44.f);
  const Edges stage = BorderEdges(One(".ps-stage"));
  EXPECT_NEAR((edges.left + edges.right) / 2.f, (stage.left + stage.right) / 2.f, 1.f) << "centred";
  EXPECT_NEAR((edges.top + edges.bottom) / 2.f, (stage.top + stage.bottom) / 2.f, 1.f);
  button->DispatchEvent(ui::EventId::Click, {});
  EXPECT_EQ(fakes_.played, std::vector<std::string>{"clip.mov"});
  EXPECT_TRUE(controller_.IsOpen()) << "the page stays under the player";
}

TEST_P(PictureSendPageTest, TheSendButtonCountsOnlyWhatCanBeSentAndTheTooLargeOnesAreMarked) {
  Arrive("a.jpg");
  Arrive("clip.mov", 80 * kMb, PictureSendKind::Video); // over the video limit
  Arrive("b.jpg");
  Frames();
  EXPECT_EQ(Text(".ps-send"), "Send (2)") << "three picked, one cannot go";
  EXPECT_EQ(Count(".ps-flag--cell"), 1U) << "only the picture that cannot go carries the label";
  // Looking at that picture shows the label on the large one too.
  Tap(".ps-cell", 1);
  EXPECT_GT(Width(One(".ps-flag--stage")), 0.f);
  EXPECT_EQ(Text(".ps-title"), "2 / 3");
  EXPECT_EQ(Text(".ps-flag--stage"), "Too large to send");
}

TEST_P(PictureSendPageTest, TheSizeShowsOnlyWithOriginalCheckedAndCheckingItMarksAPictureOverTheLimit) {
  Arrive("a.jpg", 3 * kMb);
  Arrive("b.jpg", 4 * kMb);
  Frames();
  EXPECT_EQ(Width(One(".ps-original-size")), 0.f) << "no size while the check is empty";
  EXPECT_EQ(Count(".ps-original-check--on"), 0U) << "empty when the page opens";
  EXPECT_EQ(Count(".ps-original-tick"), 0U);
  EXPECT_GE(Width(One(".ps-original-check")), 22.f - 0.5f) << "a round check, not a switch";
  EXPECT_EQ(Count(".settings-switch"), 0U);
  Tap(".ps-original-check"); // the circle itself
  EXPECT_TRUE(queue_->Original());
  EXPECT_EQ(Count(".ps-original-check--on"), 1U) << "filled";
  EXPECT_EQ(Count(".ps-original-tick"), 1U) << "with a tick";
  EXPECT_GT(Width(One(".ps-original-size")), 0.f);
  EXPECT_EQ(Text(".ps-original-size"), "7.0 MB total");
  Tap(".ps-original-label"); // the label toggles it too
  EXPECT_FALSE(queue_->Original());
  EXPECT_EQ(Count(".ps-original-check--on"), 0U);
  EXPECT_EQ(Width(One(".ps-original-size")), 0.f);
}

TEST_P(PictureSendPageTest, RemovingThePictureOnShowThroughItsBadgeUpdatesTheCountAndThePosition) {
  Fill(3);
  EXPECT_EQ(Text(".ps-send"), "Send (3)");
  Tap(".ps-badge");
  EXPECT_EQ(Text(".ps-send"), "Send (2)");
  EXPECT_EQ(Text(".ps-title"), "1 / 2");
  EXPECT_EQ(Count(".ps-cell"), 2U);
  EXPECT_EQ(Count(".ps-badge"), 1U) << "the badge follows the selected thumbnail";
  Tap(".ps-badge");
  EXPECT_EQ(Text(".ps-send"), "Send");
  EXPECT_EQ(Count(".ps-cell"), 0U) << "one left: no strip";
  EXPECT_EQ(Count(".ps-badge"), 0U) << "and nothing to remove it with: Back does that";
  EXPECT_EQ(Count("button.ps-top-btn"), 1U) << "the top bar has the back button only (no trash icon)";
  EXPECT_TRUE(controller_.IsOpen());
}

// The red badge belongs to the selected thumbnail only, hangs over its top-right corner inside the strip (not clipped),
// is a 44dp target, and does not reach the neighbouring thumbnail's centre.
TEST_P(PictureSendPageTest, TheRemoveBadgeIsOnTheSelectedThumbnailOnlyAndBigEnoughWithoutStealingTheNeighbour) {
  Fill(4);
  for (const int selected : {0, 2, 3}) {
    SCOPED_TRACE("selected: " + std::to_string(selected));
    Tap(".ps-cell", static_cast<size_t>(selected));
    ASSERT_EQ(Count(".ps-badge"), 1U);
    ui::Element* badge = nullptr;
    ui::ElementList badges;
    document_->QuerySelectorAll(badges, ".ps-badge");
    for (ui::Element* candidate : badges) {
      if (IsShown(candidate)) {
        badge = candidate;
      }
    }
    ASSERT_NE(badge, nullptr);
    const Edges edges = BorderEdges(badge);
    EXPECT_GE(edges.right - edges.left, 44.f);
    EXPECT_GE(edges.bottom - edges.top, 44.f);
    EXPECT_GE(edges.top, BorderEdges(One(".ps-strip")).top) << "not clipped at the top of the strip";
    ui::ElementList cells;
    document_->QuerySelectorAll(cells, ".ps-cell");
    std::vector<ui::Element*> shown;
    for (ui::Element* cell : cells) {
      if (IsShown(cell)) {
        shown.push_back(cell);
      }
    }
    ASSERT_EQ(shown.size(), 4U);
    const Edges mine = BorderEdges(shown[static_cast<size_t>(selected)]);
    EXPECT_GT(edges.right, mine.right) << "it hangs over the corner";
    if (selected + 1 < 4) {
      const Edges next = BorderEdges(shown[static_cast<size_t>(selected) + 1]);
      const float centre_x = (next.left + next.right) / 2.f;
      const float centre_y = (next.top + next.bottom) / 2.f;
      const bool inside =
          centre_x >= edges.left && centre_x <= edges.right && centre_y >= edges.top && centre_y <= edges.bottom;
      EXPECT_FALSE(inside) << "the neighbour's centre is its own";
    }
  }
}

TEST_P(PictureSendPageTest, TappingASmallPictureShowsItAndTheBackAndSendButtonsAreTheControllersOwn) {
  Fill(4);
  Tap(".ps-cell", 2);
  EXPECT_EQ(controller_.CurrentIndex(), 2);
  EXPECT_EQ(Text(".ps-title"), "3 / 4");
  Tap(".ps-send");
  EXPECT_TRUE(queue_->Started());
  EXPECT_FALSE(controller_.IsOpen());
}

// ---- how the chat screen wires the page (the screen cannot be built in a unit test: source pins) --------------

std::string ControllerSource() {
  return ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/ChatController.cpp");
}

std::string BodyOf(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) {
    return {};
  }
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

TEST(PictureSendWiringPinTest, EveryExitClosesThePageBeforeTheQueueLeaves) {
  const std::string source = ControllerSource();
  // Lock (Update), chat deleted, sign-out and teardown: the page's own close first, so it is not a Back that would put
  // the text back; then the queue's exit.
  const std::string update = BodyOf(source, "void ChatController::Update(");
  const size_t locked = update.find("picture_queue_->Locked();");
  ASSERT_NE(locked, std::string::npos);
  const size_t close_on_lock = update.find("picture_send_.CloseForExit();");
  ASSERT_NE(close_on_lock, std::string::npos);
  EXPECT_LT(close_on_lock, locked);
  EXPECT_NE(update.find("picture_send_.Update();"), std::string::npos) << "the picture is fitted again each frame";
  size_t at = 0;
  int exits = 0;
  while ((at = source.find("picture_queue_->SignedOut();", at)) != std::string::npos) {
    EXPECT_NE(source.substr(at > 200 ? at - 200 : 0, 200).find("picture_send_.CloseForExit();"), std::string::npos);
    ++exits;
    at += 10;
  }
  EXPECT_EQ(exits, 2) << "the profile reset and the teardown";
  const size_t deleted = source.find("picture_queue_->ChatDeleted();");
  ASSERT_NE(deleted, std::string::npos);
  EXPECT_NE(source.substr(deleted > 250 ? deleted - 250 : 0, 250).find("picture_send_.CloseForExit();"),
            std::string::npos);
}

TEST(PictureSendWiringPinTest, PendingRowsAreAddressedByIdDuringABatch) {
  // A batch keeps several rows at once, an older failed one first: front()-based Upload() / Apply(step) would act on
  // the wrong row. They are only for adding a new row; every other change goes through the id variant.
  const std::string source = ControllerSource();
  size_t at = 0;
  int upload_calls = 0;
  while ((at = source.find("pending_uploads_.Upload()", at)) != std::string::npos) {
    ++upload_calls;
    at += 10;
  }
  EXPECT_EQ(upload_calls, 1) << "only ApplyPendingUpload's thread fallback may read the first row";
  const size_t apply_step = source.find("pending_uploads_.Apply(std::move(step))");
  ASSERT_NE(apply_step, std::string::npos);
  EXPECT_EQ(source.find("pending_uploads_.Apply(std::move(step))", apply_step + 1), std::string::npos)
      << "Apply(step) is called from ApplyPendingUpload only";
  at = 0;
  while ((at = source.find("ApplyPendingUpload(", at)) != std::string::npos) {
    const std::string call = source.substr(at, 40);
    const bool is_definition = call.find("ApplyPendingUpload(PendingUploadStep") == 0;
    const bool adds_row = call.find("ApplyPendingUpload({std::move(") == 0;
    EXPECT_TRUE(is_definition || adds_row) << "a step for an existing row must use ApplyPendingUploadTo: " << call;
    at += 10;
  }
}

TEST(PictureSendWiringPinTest, EveryPictureOfAPickIsCheckedWhenItArrives) {
  const std::string arrive = BodyOf(ControllerSource(), "void ChatController::OnPhotoBatchItem(");
  ASSERT_FALSE(arrive.empty());
  EXPECT_NE(arrive.find("!MessagingReady()"), std::string::npos) << "the live lock check";
  EXPECT_NE(arrive.find("batch.session != messaging_session_"), std::string::npos);
  EXPECT_NE(arrive.find("!picks_.IsCurrent(pick_serial)"), std::string::npos);
  EXPECT_NE(arrive.find("batch.thread_id != ActiveThreadId()"), std::string::npos) << "the chat it was picked in";
  EXPECT_NE(arrive.find("drop(path)"), std::string::npos) << "a refused picture's copy is released";
  EXPECT_EQ(arrive.find("picked_picture"), std::string::npos) << "the batch never goes through the single-pick path";
}

TEST(PictureSendWiringPinTest, ThePageBackAndSendAreTheOnlyWaysOutAndAreRegistered) {
  const std::string source = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/chat/PictureSendController.cpp");
  EXPECT_NE(source.find("spec.key = \"picture_send\";"), std::string::npos);
  EXPECT_NE(source.find("spec.full_screen = true;"), std::string::npos);
  const std::string shell = ReadAll(std::string(PP_SOURCE_DIR) + "/src/gui/shell/ShellHost.cpp");
  EXPECT_NE(shell.find("key == \"picture_send\""), std::string::npos) << "the edge swipe closes the page";
}

} // namespace
} // namespace pbr
