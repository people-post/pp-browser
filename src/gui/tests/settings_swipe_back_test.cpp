// Settings pages are made of button rows. The edge swipe back used to ignore every press that started on a button, so a
// page answered only where the finger happened to land on text or empty space. With `edge_over_buttons` the left edge
// strip starts the swipe over a button too; a button anywhere else keeps its press.
#include "gui/shell/ShellSwipeBackGesture.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/EventListener.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i&, const ui::String&) override { return 0; }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 0; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class ClickCounter : public ui::EventListener {
public:
  void ProcessEvent(ui::Event&) override { ++clicks; }
  int clicks = 0;
};

constexpr const char* kSettingsLikePage =
    "<rml><body style=\"margin: 0;\">"
    "<div id=\"page\" style=\"position: absolute; left: 0px; top: 0px; width: 400px; height: 900px;\">"
    "<button id=\"row\" style=\"display: block; width: 400px; height: 60px;\">Row</button>"
    "<button id=\"inset_row\" style=\"display: block; margin: 0px 20px; width: 360px; height: 60px;\">Inset "
    "row</button>"
    "<input id=\"field\" type=\"checkbox\" style=\"display: block; width: 400px; height: 40px;\"/>"
    "<p id=\"text\" style=\"display: block; width: 400px; height: 40px;\">Footnote</p>"
    "</div></body></rml>";

class SettingsSwipeBackTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("settings-swipe-back-test", {400, 900});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(kSettingsLikePage);
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    page_ = document_->GetElementById("page");
    ASSERT_NE(page_, nullptr);
  }

  void TearDown() override {
    back_.Detach();
    if (context_) {
      ui::RemoveContext("settings-swipe-back-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }

  void AttachBack(const bool edge_over_buttons) {
    ShellSwipeBackGesture::AttachOptions options;
    options.require_edge = true; // as Settings' home and its pages: the left edge starts it
    options.axis_lock = nullptr;
    options.edge_over_buttons = edge_over_buttons;
    back_.Attach(page_, context_, std::move(options), [this]() { ++dismissed_; });
  }

  float CentreY(const char* id) {
    ui::Element* e = document_->GetElementById(id);
    return e->GetAbsoluteOffset(ui::BoxArea::Border).y + e->GetBox().GetSize(ui::BoxArea::Border).y / 2.f;
  }

  void Drag(const int from_x, const int to_x, const int y) {
    context_->ProcessMouseMove(from_x, y, 0);
    context_->ProcessMouseButtonDown(0, 0);
    for (int i = 1; i <= 10; ++i) {
      context_->ProcessMouseMove(from_x + (to_x - from_x) * i / 10, y, 0);
      context_->Update();
    }
    context_->ProcessMouseButtonUp(0, 0);
    context_->Update();
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Element* page_ = nullptr;
  ShellSwipeBackGesture back_;
  int dismissed_ = 0;
};

TEST_F(SettingsSwipeBackTest, WithoutTheOptionAPressOnAButtonRowNeverStartsTheSwipe) {
  AttachBack(false);
  Drag(8, 320, static_cast<int>(CentreY("row")));
  EXPECT_EQ(dismissed_, 0) << "the cause: a button swallows the press, so a row page does not go back";
  Drag(8, 320, static_cast<int>(CentreY("text")));
  EXPECT_EQ(dismissed_, 1) << "while the same swipe over plain text did";
}

TEST_F(SettingsSwipeBackTest, FromTheLeftEdgeASwipeOverAButtonRowGoesBack) {
  AttachBack(true);
  Drag(8, 320, static_cast<int>(CentreY("row")));
  EXPECT_EQ(dismissed_, 1);
  Drag(20, 330, static_cast<int>(CentreY("inset_row")));
  EXPECT_EQ(dismissed_, 2) << "a row inset from the edge: the finger still starts in the edge strip";
}

TEST_F(SettingsSwipeBackTest, AButtonAwayFromTheEdgeKeepsItsPress) {
  AttachBack(true);
  Drag(150, 380, static_cast<int>(CentreY("row")));
  EXPECT_EQ(dismissed_, 0) << "only the left edge strip belongs to the swipe";
}

TEST_F(SettingsSwipeBackTest, AnInputIsNeverTheStartEvenAtTheEdge) {
  AttachBack(true);
  Drag(8, 320, static_cast<int>(CentreY("field")));
  EXPECT_EQ(dismissed_, 0) << "text fields (and sliders) keep their horizontal drags";
}

TEST_F(SettingsSwipeBackTest, ATapOnARowAtTheEdgeStillClicksIt) {
  AttachBack(true);
  ClickCounter clicks;
  ui::Element* row = document_->GetElementById("row");
  row->AddEventListener(ui::EventId::Click, &clicks);
  const int y = static_cast<int>(CentreY("row"));
  context_->ProcessMouseMove(8, y, 0);
  context_->ProcessMouseButtonDown(0, 0);
  context_->ProcessMouseButtonUp(0, 0);
  context_->Update();
  EXPECT_EQ(clicks.clicks, 1) << "no drag: the press is the row's";
  EXPECT_EQ(dismissed_, 0);
  // A completed swipe does not also click the row it started on.
  Drag(8, 320, y);
  EXPECT_EQ(dismissed_, 1);
  EXPECT_EQ(clicks.clicks, 1);
  row->RemoveEventListener(ui::EventId::Click, &clicks);
}

TEST_F(SettingsSwipeBackTest, ALeftwardOrShortDragDoesNotGoBack) {
  AttachBack(true);
  Drag(8, 60, static_cast<int>(CentreY("row")));
  EXPECT_EQ(dismissed_, 0) << "short of the 30% threshold";
}

} // namespace
} // namespace pbr
