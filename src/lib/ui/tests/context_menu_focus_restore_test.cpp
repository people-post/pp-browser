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

// Kenneth 2026-11-04 (ui-mockups/09-sheets-unify): every phone menu is the small sheet of the page sheets' family:
// grab handle, centred title, grey close disc, one card of rows, red for a destructive row, no separate Cancel card.
using ContextMenuSheetTest = ContextMenuFocusRestoreTest;

namespace {

ui::Dictionary Pointer(const int x, const int y) {
  ui::Dictionary parameters;
  parameters["mouse_x"] = x;
  parameters["mouse_y"] = y;
  parameters["button"] = 0;
  return parameters;
}

std::vector<ContextMenuAction> SheetActions(std::vector<std::string>* ran) {
  std::vector<ContextMenuAction> actions;
  ContextMenuAction photo;
  photo.id = "photo";
  photo.label = "Photo";
  photo.icon = "../icons/v2/camera.svg";
  photo.run = [ran] { ran->push_back("photo"); };
  actions.push_back(photo);
  ContextMenuAction remove;
  remove.id = "remove";
  remove.label = "Remove";
  remove.danger = true;
  remove.run = [ran] { ran->push_back("remove"); };
  actions.push_back(remove);
  ContextMenuAction cancel;
  cancel.id = "cancel";
  cancel.label = "Cancel";
  cancel.cancel = true;
  cancel.run = [ran] { ran->push_back("cancel"); };
  actions.push_back(cancel);
  return actions;
}

std::vector<ui::Element*> Rows(ui::ElementDocument* document) {
  std::vector<ui::Element*> rows;
  if (ui::Element* list = document->GetElementById("context-menu-sheet-list")) {
    for (int i = 0; i < list->GetNumChildren(); ++i) {
      rows.push_back(list->GetChild(i));
    }
  }
  return rows;
}

} // namespace

TEST_F(ContextMenuSheetTest, ThePhoneSheetHasTheTitleTheDiscAndOneCardWithoutACancelCard) {
  std::vector<std::string> ran;
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().ShowActions({10, 10}, SheetActions(&ran), {}, "Send");
  context_->Update();
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--sheet"));
  ui::Element* title = document_->GetElementById("context-menu-sheet-title");
  ASSERT_NE(title, nullptr);
  EXPECT_EQ(title->GetInnerRML(), "Send");
  ui::Element* close = document_->GetElementById("context-menu-cancel");
  ASSERT_NE(close, nullptr);
  EXPECT_TRUE(close->IsClassSet("context-menu-sheet-close")) << "the grey disc, not a Cancel card";
  ui::ElementList grabbers;
  layer->GetElementsByClassName(grabbers, "context-menu-sheet-grabber");
  EXPECT_EQ(grabbers.size(), 1u);

  // The Cancel row is left out (the disc stands for it); the destructive row is red.
  const std::vector<ui::Element*> rows = Rows(document_);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_TRUE(rows[0]->IsClassSet("context-menu-item--icon"));
  EXPECT_FALSE(rows[0]->IsClassSet("context-menu-item--danger"));
  EXPECT_TRUE(rows[1]->IsClassSet("context-menu-item--danger"));

  // The close disc closes without running anything.
  close->DispatchEvent(ui::EventId::Mousedown, Pointer(390, 10));
  ContextMenuHost::Instance().Update();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_TRUE(ran.empty());
  ContextMenuHost::Instance().SetCompactLayout(false);
}

TEST_F(ContextMenuSheetTest, WithoutATitleTheRowShowsOnlyTheDisc) {
  std::vector<std::string> ran;
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().ShowActions({10, 10}, SheetActions(&ran));
  context_->Update();
  EXPECT_EQ(document_->GetElementById("context-menu-sheet-title"), nullptr);
  EXPECT_NE(document_->GetElementById("context-menu-cancel"), nullptr);
  ContextMenuHost::Instance().Dismiss();
  ContextMenuHost::Instance().SetCompactLayout(false);
}

TEST_F(ContextMenuSheetTest, ARowRunsOnItsTapNotOnThePress) {
  std::vector<std::string> ran;
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().ShowActions({10, 10}, SheetActions(&ran), {}, "Send");
  context_->Update();
  std::vector<ui::Element*> rows = Rows(document_);
  ASSERT_EQ(rows.size(), 2u);
  rows[0]->DispatchEvent(ui::EventId::Mousedown, Pointer(100, 300));
  EXPECT_TRUE(ran.empty()) << "a press alone may still become a drag";
  rows[0]->DispatchEvent(ui::EventId::Mouseup, Pointer(100, 300));
  rows[0]->DispatchEvent(ui::EventId::Click, Pointer(100, 300));
  EXPECT_EQ(ran, (std::vector<std::string>{"photo"}));
  ContextMenuHost::Instance().Update();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  ContextMenuHost::Instance().SetCompactLayout(false);
}

TEST_F(ContextMenuSheetTest, ADragDownClosesItAndRunsNothing) {
  std::vector<std::string> ran;
  bool dismissed = false;
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().ShowActions({10, 10}, SheetActions(&ran), [&dismissed] { dismissed = true; }, "Send");
  context_->Update();
  std::vector<ui::Element*> rows = Rows(document_);
  ASSERT_EQ(rows.size(), 2u);
  rows[0]->DispatchEvent(ui::EventId::Mousedown, Pointer(100, 250));
  rows[0]->DispatchEvent(ui::EventId::Mousemove, Pointer(100, 270));
  rows[0]->DispatchEvent(ui::EventId::Mousemove, Pointer(100, 390));
  rows[0]->DispatchEvent(ui::EventId::Mouseup, Pointer(100, 390));
  rows[0]->DispatchEvent(ui::EventId::Click, Pointer(100, 390));
  ContextMenuHost::Instance().Update();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_TRUE(dismissed);
  EXPECT_TRUE(ran.empty()) << "the release after a drag is no tap";
  ContextMenuHost::Instance().SetCompactLayout(false);
}

TEST_F(ContextMenuSheetTest, ALongPressMenuOnThePhoneIsTheSheetWithItsTitleAndReactionsAbove) {
  // Providers stay registered on the shared host: nothing here may point into this test's stack.
  static std::vector<std::string> ran;
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().RegisterProvider([](const ContextMenuRequest& request) {
    std::vector<ContextMenuAction> actions;
    if (!request.target || request.target->GetId() != "other") {
      return actions;
    }
    ContextMenuAction like;
    like.id = "like";
    like.label = "+1";
    like.quick = true;
    like.run = [] { ran.push_back("like"); };
    actions.push_back(like);
    ContextMenuAction reply;
    reply.id = "reply";
    reply.label = "Reply";
    reply.icon = "../icons/v2/reply.svg";
    reply.run = [] { ran.push_back("reply"); };
    actions.push_back(reply);
    return actions;
  });
  ContextMenuHost::Instance().SetAnchorResolver([](ui::Element* target) -> ui::Element* {
    return target && target->GetId() == "other" ? target : nullptr;
  });
  ContextMenuHost::Instance().SetTitleResolver([](const ContextMenuRequest&) { return std::string("Message"); });
  ContextMenuHost::Instance().OnLongPress({20, 380}, document_->GetElementById("other"));
  context_->Update();
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--sheet"));
  ASSERT_NE(document_->GetElementById("context-menu-sheet-title"), nullptr);
  EXPECT_EQ(document_->GetElementById("context-menu-sheet-title")->GetInnerRML(), "Message");
  ui::ElementList quick;
  layer->GetElementsByClassName(quick, "context-menu-sheet-quick");
  ASSERT_EQ(quick.size(), 1u);
  EXPECT_EQ(quick[0]->GetNumChildren(), 1);
  EXPECT_EQ(Rows(document_).size(), 1u);
  ContextMenuHost::Instance().Dismiss();
  ContextMenuHost::Instance().SetTitleResolver({});
  ContextMenuHost::Instance().SetAnchorResolver({});
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
