// The "Forwarding 2/7" page drawn by the real forward_progress.rml and the real stylesheets, with the controller's own
// data model: what shows while an item uploads, while a Cancel waits for the item under way, and when Close is on offer.
#include "gui/chat/ForwardProgressController.h"
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

std::string Asset(const char* relative) {
  std::ifstream in(std::string(PP_BROWSER_ASSETS_DIR) + "/" + relative, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  std::string text = out.str();
  std::erase(text, '\r'); // a Windows checkout has CRLF line ends
  return text;
}

class ForwardProgressPageTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("forward-progress-page-test", {390, 844});
    ASSERT_NE(context, nullptr);
    ASSERT_TRUE(controller.RegisterModel(context));
    ForwardProgressController::Handlers handlers;
    handlers.cancel = [this]() { ++cancels; };
    handlers.close = [this]() { ++closes; };
    controller.SetHandlers(handlers);
    const std::string rml = "<rml><head><style>" + Asset("themes/components.rcss") + Asset("themes/colors-light.rcss") +
                            "</style></head><body style=\"padding: 0;\"><div style=\"position: absolute; top: 0; left: "
                            "0; width: 390dp; height: 844dp;\">" +
                            LocalizationService::Instance().LocalizeText(Asset("views/forward_progress.rml")) +
                            "</div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("forward-progress-page-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  void Draw(const ForwardProgressView& view) {
    controller.Show(view);
    context->Update();
    context->Update();
  }
  std::vector<ui::Element*> Shown(const char* cls) {
    ui::ElementList found;
    document->GetElementsByClassName(found, cls);
    std::vector<ui::Element*> shown;
    for (ui::Element* element : found) {
      if (element->IsVisible(true)) {
        shown.push_back(element);
      }
    }
    return shown;
  }
  void Tap(ui::Element* element) {
    const ui::Vector2f at = element->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
    context->ProcessMouseMove(static_cast<int>(at.x + size.x / 2), static_cast<int>(at.y + size.y / 2), 0);
    context->ProcessMouseButtonDown(0, 0);
    context->ProcessMouseButtonUp(0, 0);
    context->Update();
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
  ForwardProgressController controller;
  int cancels = 0;
  int closes = 0;
};

TEST_F(ForwardProgressPageTest, ShowsTheTitleAnActivityRingAndCancelWhileAnItemUploadsWithNoFigure) {
  ForwardProgressView view;
  view.title = "Forwarding 2/7";
  view.line = "Sending… 4s";
  Draw(view);
  ASSERT_EQ(Shown("forward-progress-title").size(), 1u);
  EXPECT_EQ(std::string(Shown("forward-progress-title")[0]->GetInnerRML().c_str()), "Forwarding 2/7");
  EXPECT_EQ(Shown("forward-progress-spinner").size(), 1u) << "no figure yet: activity";
  EXPECT_TRUE(Shown("forward-progress-bar").empty());
  EXPECT_TRUE(Shown("forward-progress-ring").empty());
  EXPECT_EQ(Shown("forward-progress-stay").size(), 1u) << "the line that says leaving stops it";
  ASSERT_EQ(Shown("forward-progress-btn").size(), 1u) << "Cancel only";
  Tap(Shown("forward-progress-btn")[0]);
  EXPECT_EQ(cancels, 1);
  EXPECT_EQ(closes, 0);
}

TEST_F(ForwardProgressPageTest, ShowsTheUploadRingWithThePercentInsideNotABar) {
  ForwardProgressView view;
  view.title = "Forwarding 1/3";
  view.percent = 42;
  Draw(view);
  ASSERT_EQ(Shown("forward-progress-ring").size(), 1u);
  EXPECT_TRUE(Shown("forward-progress-bar").empty()) << "no horizontal bar any more";
  EXPECT_TRUE(Shown("forward-progress-spinner").empty());
  ASSERT_EQ(Shown("forward-progress-ring-text").size(), 1u);
  EXPECT_EQ(std::string(Shown("forward-progress-ring-text")[0]->GetInnerRML().c_str()), "42%");
  ui::Element* ring = Shown("forward-progress-ring")[0];
  const ui::Vector2f ring_at = ring->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f ring_size = ring->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_NEAR(ring_size.x, 72.f, 1.f);
  EXPECT_NEAR(ring_size.y, 72.f, 1.f);
  EXPECT_NEAR(ring_at.x + ring_size.x / 2, 195.f, 1.5f) << "centred on the page";
  // The filled part is the chat ring's conic-gradient decorator, set from the model.
  ASSERT_EQ(Shown("forward-progress-ring-track").size(), 1u);
  const std::string decorator = Shown("forward-progress-ring-track")[0]->GetProperty("decorator")->ToString().c_str();
  EXPECT_NE(decorator.find("conic-gradient"), std::string::npos) << decorator;
  EXPECT_NE(decorator.find("42%"), std::string::npos) << decorator;
  ui::Element* text = Shown("forward-progress-ring-text")[0];
  const ui::Vector2f t_at = text->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f t_size = text->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_GE(t_at.x, ring_at.x);
  EXPECT_LE(t_at.x + t_size.x, ring_at.x + ring_size.x) << "the figure is inside the ring";

  // Switching to the unknown amount: the spinner takes the ring's place and the page keeps its height.
  const ui::Vector2f actions_before = Shown("forward-progress-actions")[0]->GetAbsoluteOffset(ui::BoxArea::Border);
  view.percent = -1;
  view.line = "Sending… 3s";
  Draw(view);
  ASSERT_EQ(Shown("forward-progress-spinner").size(), 1u);
  EXPECT_TRUE(Shown("forward-progress-ring").empty());
  ui::Element* spinner = Shown("forward-progress-spinner")[0];
  const ui::Vector2f spin_at = spinner->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f spin_size = spinner->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_NEAR(spin_at.x, ring_at.x, 1.f);
  EXPECT_NEAR(spin_at.y, ring_at.y, 1.f);
  EXPECT_NEAR(spin_size.x, ring_size.x, 1.f);
  EXPECT_NEAR(spin_size.y, ring_size.y, 1.f);
  EXPECT_NEAR(Shown("forward-progress-actions")[0]->GetAbsoluteOffset(ui::BoxArea::Border).y, actions_before.y, 1.f)
      << "the buttons do not move between the two states";
  view.percent = 40;
  view.line = "";
  Draw(view);

  view.finishing = true;
  view.percent = -1;
  view.line = "Finishing — the current item will still go through…";
  Draw(view);
  EXPECT_TRUE(Shown("forward-progress-btn").empty()) << "Cancel is gone: the item under way is being waited for";
  EXPECT_TRUE(Shown("forward-progress-stay").empty());

  view.can_close = true;
  Draw(view);
  ASSERT_EQ(Shown("forward-progress-btn").size(), 1u) << "after 15 s: Close";
  Tap(Shown("forward-progress-btn")[0]);
  EXPECT_EQ(closes, 1);
  EXPECT_EQ(cancels, 0);
}

TEST_F(ForwardProgressPageTest, TheButtonsAreAtLeast44dpHighAndInsideThePhone) {
  ForwardProgressView view;
  view.title = "Forwarding 1/1";
  Draw(view);
  ui::Element* button = Shown("forward-progress-btn").at(0);
  const ui::Vector2f at = button->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = button->GetBox().GetSize(ui::BoxArea::Border);
  EXPECT_GE(size.y, 44.f);
  EXPECT_GE(at.x, 12.f);
  EXPECT_LE(at.x + size.x, 390.f - 12.f);
}

} // namespace
} // namespace pbr
