#include "lib/ui/ContextMenuHost.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

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

// The menu remembers which element had the focus so that closing it can give the focus back. That element can be
// destroyed while the menu is open (a pane remount); closing the menu then crashed in Element::Focus on the iOS
// simulator (2026-11-03, ContextMenuHost::Dismiss -> Element::Focus -> ComputedValues::focus).
class ContextMenuFocusRestoreTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("context-menu-focus-test", {400, 400});
    ASSERT_NE(context_, nullptr);
    document_ = context_->LoadDocumentFromMemory(
        "<rml><head><style>body { width: 400px; height: 400px; } input { width: 200px; height: 20px; }</style></head>"
        "<body><div id='pane'><input id='field' type='text'/></div><div id='other'/></body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    ContextMenuHost::Instance().Install(context_);
  }

  void TearDown() override {
    ContextMenuHost::Instance().Dismiss();
    ContextMenuHost::Instance().Install(nullptr);
    if (context_) { ui::RemoveContext("context-menu-focus-test"); }
    if (initialized_) { ui::Shutdown(); }
    ui::SetRenderInterface(nullptr);
  }

  void OpenMenu() {
    std::vector<ContextMenuAction> actions;
    ContextMenuAction action;
    action.id = "noop";
    action.label = "Noop";
    action.run = [] {};
    actions.push_back(std::move(action));
    ContextMenuHost::Instance().ShowActions({10, 10}, std::move(actions));
    context_->Update();
  }

  NullRender renderer_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
};

TEST_F(ContextMenuFocusRestoreTest, ClosingTheMenuGivesTheFocusBackToTheField) {
  ui::Element* field = document_->GetElementById("field");
  ASSERT_NE(field, nullptr);
  field->Focus();
  ASSERT_EQ(context_->GetFocusElement(), field);

  OpenMenu();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  ASSERT_TRUE(ContextMenuHost::Instance().HandleDismiss());

  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(context_->GetFocusElement(), field);
}

TEST_F(ContextMenuFocusRestoreTest, ClosingTheMenuAfterTheFocusedFieldWasRemovedDoesNotCrash) {
  ui::Element* field = document_->GetElementById("field");
  ASSERT_NE(field, nullptr);
  field->Focus();

  OpenMenu();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());

  // The pane is remounted while the menu is open: the field it remembered is destroyed.
  ui::Element* pane = document_->GetElementById("pane");
  ASSERT_NE(pane, nullptr);
  { ui::ElementPtr removed = pane->RemoveChild(field); }
  context_->Update();

  EXPECT_TRUE(ContextMenuHost::Instance().HandleDismiss());
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
}

// The phone Contacts "+" opens a pull-down under the button (2026-11-03), not the bottom action sheet that
// ShowActions gives on a phone; the sheet stays for the other phone menus.
using ContextMenuPopoverTest = ContextMenuFocusRestoreTest;

std::vector<ContextMenuAction> OneAction() {
  std::vector<ContextMenuAction> actions;
  ContextMenuAction action;
  action.id = "noop";
  action.label = "Noop";
  action.run = [] {};
  actions.push_back(std::move(action));
  return actions;
}

TEST_F(ContextMenuPopoverTest, APopoverOnThePhoneIsAPullDownNotASheet) {
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().ShowActionsPopover(document_->GetElementById("field"), OneAction());
  context_->Update();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--popover"));
  EXPECT_FALSE(layer->IsClassSet("context-menu-layer--sheet"));
  EXPECT_EQ(document_->GetElementById("context-menu-cancel"), nullptr);
  ContextMenuHost::Instance().Dismiss();

  // Plain ShowActions on the phone is still the sheet, and the popover flag does not stick.
  ContextMenuHost::Instance().ShowActions({10, 10}, OneAction());
  context_->Update();
  layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--sheet"));
  EXPECT_FALSE(layer->IsClassSet("context-menu-layer--popover"));
  ContextMenuHost::Instance().Dismiss();
  ContextMenuHost::Instance().SetCompactLayout(false);
}

TEST_F(ContextMenuPopoverTest, APopoverOnTheDesktopIsAPlainFloatMenu) {
  ContextMenuHost::Instance().ShowActionsPopover(document_->GetElementById("field"), OneAction());
  context_->Update();
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  EXPECT_FALSE(layer->IsClassSet("context-menu-layer--popover"));
  EXPECT_FALSE(layer->IsClassSet("context-menu-layer--sheet"));
}

}  // namespace
}  // namespace pbr
