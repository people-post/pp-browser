#include "lib/ui/ContextMenuHost.h"

#include <ui/base/SystemInterface.h>
#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>
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
        "<rml><head><style>body { width: 400px; height: 400px; } input { width: 200px; height: 20px; } "
        ".context-menu-layer { position: fixed; top: 0; left: 0; right: 0; bottom: 0; } "
        ".context-menu-sheet { position: absolute; }</style></head>"
        "<body><div id='pane'><input id='field' type='text'/></div><div id='other'/></body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    ContextMenuHost::Instance().Install(context_);
  }

  void TearDown() override {
    ContextMenuHost::Instance().Dismiss();
    ContextMenuHost::Instance().Install(nullptr);
    if (context_) {
      ui::RemoveContext("context-menu-focus-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
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
  {
    ui::ElementPtr removed = pane->RemoveChild(field);
  }
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

  // Like iOS's context menu (Kenneth 2026-11-04): the label on the left, a small 18dp icon on the right.
  ui::ElementList bodies;
  rows[0]->GetElementsByClassName(bodies, "context-menu-item-body");
  ASSERT_EQ(bodies.size(), 1u);
  ui::Element* body = bodies[0];
  ASSERT_GE(body->GetNumChildren(), 2);
  EXPECT_TRUE(body->GetChild(0)->IsClassSet("context-menu-item-label"));
  ui::Element* icon = body->GetChild(body->GetNumChildren() - 1);
  EXPECT_TRUE(icon->IsClassSet("context-menu-item-icon")) << "the icon trails the label";
  ASSERT_EQ(icon->GetNumChildren(), 1);
  EXPECT_EQ(icon->GetChild(0)->GetAttribute<ui::String>("width", ""), "18");

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
  ContextMenuHost::Instance().SetAnchorResolver(
      [](ui::Element* target) -> ui::Element* { return target && target->GetId() == "other" ? target : nullptr; });
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

// Kenneth 2026-11-04 (PLAN-ui-refresh-impl 62.7): the reactions scroll sideways, and "+" grows the same sheet to
// every emoji instead of opening a full-screen window.
TEST_F(ContextMenuSheetTest, ThePlusGrowsTheSheetAndHandsItsBodyOver) {
  static ui::Element* handed = nullptr;
  static std::function<void()> close_sheet;
  static int ran_plus = 0;
  handed = nullptr;
  close_sheet = nullptr;
  ran_plus = 0;
  std::vector<ContextMenuAction> actions;
  for (const char* emoji : {"A", "B", "C"}) {
    ContextMenuAction react;
    react.id = emoji;
    react.label = emoji;
    react.quick = true;
    react.run = [] {};
    actions.push_back(react);
  }
  ContextMenuAction more;
  more.id = "more";
  more.label = "+";
  more.quick = true;
  more.run = [] { ++ran_plus; };
  more.expand = [](ui::Element* mount, std::function<void()> done) {
    handed = mount;
    close_sheet = std::move(done);
    mount->SetInnerRML("<button id='pick'>X</button>");
  };
  actions.push_back(more);
  ContextMenuAction reply;
  reply.id = "reply";
  reply.label = "Reply";
  reply.run = [] {};
  actions.push_back(reply);

  bool dismissed = false;
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().ShowActions({10, 10}, std::move(actions), [&dismissed] { dismissed = true; }, "Message");
  context_->Update();
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  ui::ElementList scrolls;
  layer->GetElementsByClassName(scrolls, "context-menu-sheet-quick-scroll");
  ASSERT_EQ(scrolls.size(), 1u);
  EXPECT_EQ(scrolls[0]->GetNumChildren(), 3) << "the reactions scroll";
  ui::Element* quick = scrolls[0]->GetParentNode();
  ASSERT_EQ(quick->GetNumChildren(), 2);
  ui::Element* plus = quick->GetChild(1);
  EXPECT_EQ(plus->GetInnerRML().find("+") != ui::String::npos, true) << "the + sits outside the scroll, at the end";

  plus->DispatchEvent(ui::EventId::Mousedown, Pointer(380, 300));
  plus->DispatchEvent(ui::EventId::Mouseup, Pointer(380, 300));
  plus->DispatchEvent(ui::EventId::Click, Pointer(380, 300));
  ContextMenuHost::Instance().Update();
  EXPECT_TRUE(ContextMenuHost::Instance().IsOpen()) << "the sheet stays, grown";
  EXPECT_EQ(ran_plus, 0) << "the + does not run on the phone";
  ui::Element* panel = document_->GetElementById("context-menu-panel");
  ASSERT_NE(panel, nullptr);
  EXPECT_TRUE(panel->IsClassSet("context-menu-sheet--expanded"));
  ASSERT_NE(handed, nullptr);
  EXPECT_EQ(handed->GetId(), "context-menu-sheet-expand");
  ASSERT_NE(document_->GetElementById("pick"), nullptr);

  // A click inside the body reaches its own button and does not close the sheet by itself.
  document_->GetElementById("pick")->DispatchEvent(ui::EventId::Click, Pointer(200, 300));
  ContextMenuHost::Instance().Update();
  EXPECT_TRUE(ContextMenuHost::Instance().IsOpen());
  ASSERT_TRUE(close_sheet);
  close_sheet(); // the body's pick
  ContextMenuHost::Instance().Update();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_FALSE(dismissed) << "a pick is a choice, not a dismissal";
  close_sheet = nullptr;
  ContextMenuHost::Instance().SetCompactLayout(false);
}

// The Home composer's plus: a floating pull-down opened above the button (two rows), closed by a dismiss.
TEST_F(ContextMenuPopoverTest, ALeftAlignedPopoverIsAFloatMenuThatClosesOnDismiss) {
  ContextMenuHost::Instance().SetCompactLayout(true);
  std::vector<ContextMenuAction> actions = OneAction();
  actions.push_back(actions.front());
  actions[1].id = "second";
  ContextMenuHost::Instance().ShowActionsPopoverLeftAligned(document_->GetElementById("field"), std::move(actions));
  context_->Update();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--popover"));
  EXPECT_FALSE(layer->IsClassSet("context-menu-layer--sheet"));
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--popover-left"));
  ui::ElementList items;
  layer->GetElementsByClassName(items, "context-menu-item");
  EXPECT_EQ(items.size(), 2u) << "both rows are in the menu";
  EXPECT_TRUE(ContextMenuHost::Instance().HandleDismiss());
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  ContextMenuHost::Instance().SetCompactLayout(false);
}

// A left-aligned show issued from a menu item's click runs while the first menu's teardown is pending; the re-issued
// show must keep its left alignment (the card would otherwise open as a right-edge pull-down, partly off-screen).
TEST_F(ContextMenuPopoverTest, ALeftAlignedShowDuringAPendingTeardownStaysLeftAligned) {
  ContextMenuHost::Instance().SetCompactLayout(true);
  // The first menu is a pull-down too (a phone's plain menu is the sheet, whose rows run on their own timing).
  ui::Element* field = document_->GetElementById("field");
  std::vector<ContextMenuAction> first = OneAction();
  bool ran = false;
  first.front().run = [field, &ran] {
    ran = true;
    ContextMenuHost::Instance().ShowActionsPopoverLeftAligned(field, OneAction());
  };
  ContextMenuHost::Instance().ShowActionsPopover(field, std::move(first));
  context_->Update();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  ui::ElementList items;
  document_->GetElementById("context-menu-layer")->GetElementsByClassName(items, "context-menu-item");
  ASSERT_FALSE(items.empty());
  items[0]->DispatchEvent(ui::EventId::Mousedown, Pointer(10, 10));
  context_->Update();
  ASSERT_TRUE(ran) << "the row's click ran its action";
  ContextMenuHost::Instance().Update();
  context_->Update();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--popover"));
  EXPECT_TRUE(layer->IsClassSet("context-menu-layer--popover-left"));
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

} // namespace
} // namespace pbr

namespace pbr {
namespace {

// G017 / #120: a choice can carry a one-line explanation under its title, and the current one is checked.
TEST_F(ContextMenuPopoverTest, ARowWithADetailShowsItUnderItsTitleAndTheCurrentOneIsChecked) {
  ContextMenuHost::Instance().SetCompactLayout(true);
  std::vector<ContextMenuAction> actions = OneAction();
  actions[0].label = "Everyone";
  actions[0].detail = "Anyone can add people";
  actions[0].selected = true;
  ContextMenuAction other;
  other.id = "other";
  other.label = "Admins only";
  other.detail = "Members only chat";
  other.run = [] {};
  actions.push_back(std::move(other));
  ContextMenuHost::Instance().ShowActions({10, 10}, std::move(actions));
  context_->Update();
  ui::Element* layer = document_->GetElementById("context-menu-layer");
  ASSERT_NE(layer, nullptr);
  const std::string rml = layer->GetInnerRML();
  EXPECT_NE(rml.find("Everyone"), std::string::npos);
  EXPECT_NE(rml.find("Anyone can add people"), std::string::npos);
  EXPECT_NE(rml.find("Admins only"), std::string::npos);
  EXPECT_NE(rml.find("Members only chat"), std::string::npos);
  EXPECT_EQ(rml.find("✓"), rml.rfind("✓")) << "only the current one is checked";
  EXPECT_NE(rml.find("✓"), std::string::npos);
  ContextMenuHost::Instance().Dismiss();
  ContextMenuHost::Instance().SetCompactLayout(false);
}

} // namespace

// The keyboard can hide (or show) while the bottom sheet is open: the sheet's `bottom` is a pixel value written when it
// opened, so it must be written again when the inset changes (iPhone, 2026-11-12: the sheet hung one keyboard height
// above the screen bottom after the keyboard went away).
using ContextMenuBottomInsetTest = ContextMenuFocusRestoreTest;

namespace {

std::string PanelBottom(ui::ElementDocument* document) {
  ui::Element* panel = document->GetElementById("context-menu-panel");
  const ui::Property* bottom = panel ? panel->GetProperty("bottom") : nullptr;
  return bottom ? bottom->ToString() : std::string();
}

} // namespace

TEST_F(ContextMenuBottomInsetTest, TheSheetDropsWhenTheKeyboardHidesWhileItIsOpen) {
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().SetBottomInsetDp(300);
  ContextMenuHost::Instance().ShowActions({10, 10}, OneAction());
  context_->Update();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  const std::string raised = PanelBottom(document_);
  ASSERT_FALSE(raised.empty());

  ContextMenuHost::Instance().SetBottomInsetDp(0);
  const std::string lowered = PanelBottom(document_);
  EXPECT_NE(lowered, raised);
  EXPECT_EQ(lowered, "0px");
  ContextMenuHost::Instance().SetBottomInsetDp(0);
  ContextMenuHost::Instance().SetCompactLayout(false);
}

TEST_F(ContextMenuBottomInsetTest, TheSheetRisesWhenTheKeyboardShowsWhileItIsOpen) {
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().SetBottomInsetDp(0);
  ContextMenuHost::Instance().ShowActions({10, 10}, OneAction());
  context_->Update();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  ASSERT_EQ(PanelBottom(document_), "0px");

  ContextMenuHost::Instance().SetBottomInsetDp(300);
  EXPECT_NE(PanelBottom(document_), "0px");
  ContextMenuHost::Instance().SetBottomInsetDp(0);
  ContextMenuHost::Instance().SetCompactLayout(false);
}

TEST_F(ContextMenuBottomInsetTest, ASameValueDoesNotLayTheSheetOutAgain) {
  ContextMenuHost::Instance().SetCompactLayout(true);
  ContextMenuHost::Instance().SetBottomInsetDp(300);
  ContextMenuHost::Instance().ShowActions({10, 10}, OneAction());
  context_->Update();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  ui::Element* panel = document_->GetElementById("context-menu-panel");
  ASSERT_NE(panel, nullptr);

  // Overwrite the value by hand: a repeated call with the same inset must leave it alone.
  panel->SetProperty("bottom", "7px");
  ContextMenuHost::Instance().SetBottomInsetDp(300);
  EXPECT_EQ(PanelBottom(document_), "7px");
  ContextMenuHost::Instance().SetBottomInsetDp(0);
  ContextMenuHost::Instance().SetCompactLayout(false);
}

namespace {

float PanelHeightPx(ui::ElementDocument* document) {
  ui::Element* panel = document->GetElementById("context-menu-panel");
  const ui::Property* height = panel ? panel->GetProperty("height") : nullptr;
  return height ? std::strtof(height->ToString().c_str(), nullptr) : -1.f;
}

} // namespace

// Opens the compact sheet with a reactions row whose "+" expands it, then taps the "+".
class ContextMenuExpandedInsetTest : public ContextMenuBottomInsetTest {
protected:
  void OpenExpanded(const int inset_dp) {
    ContextMenuHost::Instance().SetCompactLayout(true);
    ContextMenuHost::Instance().SetBottomInsetDp(inset_dp);
    ContextMenuAction more;
    more.id = "more";
    more.label = "+";
    more.quick = true;
    more.run = [] {};
    more.expand = [](ui::Element* mount, std::function<void()>) { mount->SetInnerRML("<div>X</div>"); };
    ContextMenuAction reply;
    reply.id = "reply";
    reply.label = "Reply";
    reply.run = [] {};
    ContextMenuHost::Instance().ShowActions({10, 10}, {more, reply}, nullptr, "Message");
    context_->Update();
    ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
    ui::ElementList quicks;
    document_->GetElementById("context-menu-layer")->GetElementsByClassName(quicks, "context-menu-sheet-quick");
    ASSERT_FALSE(quicks.empty());
    ui::Element* plus = quicks[0]->GetChild(quicks[0]->GetNumChildren() - 1);
    plus->DispatchEvent(ui::EventId::Mousedown, Pointer(380, 300));
    plus->DispatchEvent(ui::EventId::Mouseup, Pointer(380, 300));
    plus->DispatchEvent(ui::EventId::Click, Pointer(380, 300));
    ContextMenuHost::Instance().Update();
    ASSERT_TRUE(document_->GetElementById("context-menu-panel")->IsClassSet("context-menu-sheet--expanded"));
  }

  void TearDown() override {
    ContextMenuHost::Instance().SetBottomInsetDp(0);
    ContextMenuHost::Instance().SetCompactLayout(false);
    ContextMenuBottomInsetTest::TearDown();
  }
};

TEST_F(ContextMenuExpandedInsetTest, TheExpandedSheetGrowsWhenTheKeyboardHides) {
  OpenExpanded(100);
  const float with_keyboard = PanelHeightPx(document_);
  ASSERT_GT(with_keyboard, 0.f);
  ContextMenuHost::Instance().SetBottomInsetDp(0);
  EXPECT_GT(PanelHeightPx(document_), with_keyboard);
  EXPECT_TRUE(document_->GetElementById("context-menu-panel")->IsClassSet("context-menu-sheet--expanded"));
}

TEST_F(ContextMenuExpandedInsetTest, TheExpandedSheetShrinksWhenTheKeyboardShows) {
  OpenExpanded(0);
  const float without_keyboard = PanelHeightPx(document_);
  ASSERT_GT(without_keyboard, 0.f);
  ContextMenuHost::Instance().SetBottomInsetDp(100);
  EXPECT_LT(PanelHeightPx(document_), without_keyboard);
  EXPECT_TRUE(document_->GetElementById("context-menu-panel")->IsClassSet("context-menu-sheet--expanded"));
}

TEST_F(ContextMenuExpandedInsetTest, ASameValueLeavesTheExpandedHeightAlone) {
  OpenExpanded(100);
  ui::Element* panel = document_->GetElementById("context-menu-panel");
  panel->SetProperty("height", "7px");
  ContextMenuHost::Instance().SetBottomInsetDp(100);
  EXPECT_EQ(PanelHeightPx(document_), 7.f);
}

TEST_F(ContextMenuBottomInsetTest, ChangingTheInsetWithoutAnOpenMenuIsHarmless) {
  ASSERT_FALSE(ContextMenuHost::Instance().IsOpen());
  ContextMenuHost::Instance().SetBottomInsetDp(300);
  ContextMenuHost::Instance().SetBottomInsetDp(0);
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
}

// iPhone, 2026-11-12 (Kenneth): typing, send, then a long press on the message just sent while the keyboard is still
// up. The message sheet opened above the keyboard, then the keyboard went down and the sheet fell with it; Edit brought
// the keyboard back up. The press on the message held the focus back from the composer until the finger lifted (a
// touch that turns into a scroll keeps the keyboard); after a long press the lift still moved it, the composer lost
// the focus and the keyboard closed. The keyboard now stays as it was while the sheet is open.
namespace {

// The platform: a clock the test moves, and the on-screen keyboard text input starts and stops.
class FakeKeyboard : public ui::SystemInterface {
public:
  double GetElapsedTime() override { return now; }
  void ActivateKeyboard(ui::Vector2f, float, ui::KeyboardType) override {
    ++activations;
    up = true;
  }
  void DeactivateKeyboard() override {
    ++deactivations;
    up = false;
  }
  double now = 100.0;
  int activations = 0;
  int deactivations = 0;
  bool up = false;
};

// The message menu's actions, set by the test running (the host keeps its providers for the whole process).
std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)>* g_message_actions = nullptr;

constexpr int kKeyboardDp = 300;

} // namespace

class ContextMenuKeyboardTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer_);
    ui::SetSystemInterface(&keyboard_);
    initialized_ = ui::Initialise();
    ASSERT_TRUE(initialized_);
    context_ = ui::CreateContext("context-menu-keyboard-test", {400, 800});
    ASSERT_NE(context_, nullptr);
    // As in the chat: message bubbles take no focus (components.rcss), their row does.
    document_ = context_->LoadDocumentFromMemory(
        "<rml><head><style>body { display: block; width: 400px; height: 800px; margin: 0; } div { display: block; } "
        "#msg { position: absolute; top: 40px; left: 0; width: 400px; height: 60px; } "
        ".bubble { width: 200px; height: 40px; focus: none; } "
        "#draft { position: absolute; top: 700px; left: 0; width: 300px; height: 40px; } "
        "button { display: block; height: 50px; } "
        ".context-menu-layer { position: fixed; top: 0; left: 0; right: 0; bottom: 0; } "
        ".context-menu-scrim { position: absolute; top: 0; left: 0; right: 0; bottom: 0; } "
        ".context-menu-sheet { position: absolute; }</style></head>"
        "<body><div id='msg' message-id='m1'><div class='bubble' id='bubble'>hello</div></div>"
        "<textarea id='draft'></textarea></body></rml>");
    ASSERT_NE(document_, nullptr);
    document_->Show();
    context_->Update();
    msg_ = document_->GetElementById("msg");
    draft_ = document_->GetElementById("draft");
    ASSERT_NE(msg_, nullptr);
    ASSERT_NE(draft_, nullptr);

    ContextMenuHost& host = ContextMenuHost::Instance();
    host.Install(context_);
    host.SetCompactLayout(true);
    host.SetAnchorResolver([](ui::Element* target) -> ui::Element* {
      for (ui::Element* cur = target; cur; cur = cur->GetParentNode()) {
        if (cur->HasAttribute("message-id")) {
          return cur;
        }
      }
      return nullptr;
    });
    static const bool registered = [] {
      ContextMenuHost::Instance().RegisterProvider([](const ContextMenuRequest& request) {
        return g_message_actions ? (*g_message_actions)(request) : std::vector<ContextMenuAction>{};
      });
      return true;
    }();
    (void)registered;
    message_actions_ = [this](const ContextMenuRequest& request) {
      std::vector<ContextMenuAction> actions;
      if (!request.target || (request.target != msg_ && !msg_->Contains(request.target))) {
        return actions;
      }
      ContextMenuAction edit;
      edit.id = "edit_message";
      edit.label = "Edit";
      edit.run = [this] { // as ChatController::StartEditMessage: the words go into the composer, which takes the focus
        ++edits_;
        draft_->Focus();
      };
      actions.push_back(std::move(edit));
      ContextMenuAction copy;
      copy.id = "copy_message";
      copy.label = "Copy";
      copy.run = [this] { ++copies_; };
      actions.push_back(std::move(copy));
      return actions;
    };
    g_message_actions = &message_actions_;
  }

  void TearDown() override {
    g_message_actions = nullptr;
    ContextMenuHost& host = ContextMenuHost::Instance();
    host.Dismiss();
    host.SetAnchorResolver(nullptr);
    host.SetBottomInsetDp(0);
    host.SetCompactLayout(false);
    host.Install(nullptr);
    if (context_) {
      ui::RemoveContext("context-menu-keyboard-test");
    }
    if (initialized_) {
      ui::Shutdown();
    }
    ui::SetSystemInterface(nullptr);
    ui::SetRenderInterface(nullptr);
  }

  /** One UI turn in the app's order (Application's loop): the menu host, the inset the platform reports for the
   *  keyboard (ShellHost::ApplySafeAreaLayout passes it on to the sheet), then the context. */
  void Frame(const double seconds = 1.0 / 60.0) {
    keyboard_.now += seconds;
    ContextMenuHost::Instance().Update();
    ContextMenuHost::Instance().SetBottomInsetDp(keyboard_.up ? kKeyboardDp : 0);
    context_->Update();
  }

  /** The composer has the focus and the keyboard is up. */
  void StartTyping() {
    ASSERT_TRUE(draft_->Focus());
    Frame();
    ASSERT_TRUE(keyboard_.up);
    ASSERT_EQ(context_->GetFocusElement(), draft_);
    keyboard_.activations = 0;
    keyboard_.deactivations = 0;
  }

  /** A finger held on the message's bubble past the long-press time; the sheet opens under it. */
  void PressAndHoldMessage() {
    context_->ProcessTouchStart(ui::TouchList{touch_}, 0);
    Frame();
    Frame(0.6);
  }

  void Lift() { context_->ProcessTouchEnd(ui::TouchList{touch_}, 0); }

  ui::Element* SheetRow(const std::string& label) {
    ui::Element* layer = document_->GetElementById("context-menu-layer");
    if (!layer) {
      return nullptr;
    }
    ui::ElementList rows;
    layer->GetElementsByClassName(rows, "context-menu-item");
    for (ui::Element* row : rows) {
      if (row->GetInnerRML().find(label) != std::string::npos) {
        return row;
      }
    }
    return nullptr;
  }

  void Tap(ui::Element* element) {
    element->DispatchEvent(ui::EventId::Mousedown, Pointer(100, 600));
    element->DispatchEvent(ui::EventId::Mouseup, Pointer(100, 600));
    element->DispatchEvent(ui::EventId::Click, Pointer(100, 600));
  }

  NullRender renderer_;
  FakeKeyboard keyboard_;
  bool initialized_ = false;
  ui::Context* context_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Element* msg_ = nullptr;
  ui::Element* draft_ = nullptr;
  ui::Touch touch_{1, {50.f, 60.f}};
  std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)> message_actions_;
  int edits_ = 0;
  int copies_ = 0;
};

TEST_F(ContextMenuKeyboardTest, ALongPressWhileTypingKeepsTheKeyboardAndTheSheetStill) {
  StartTyping();
  PressAndHoldMessage();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  const std::string opened_at = PanelBottom(document_);
  ASSERT_EQ(opened_at, "300px");

  Lift();
  for (int i = 0; i < 10; ++i) {
    Frame();
    EXPECT_EQ(PanelBottom(document_), opened_at) << "frame " << i;
  }
  EXPECT_TRUE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(keyboard_.deactivations, 0);
  EXPECT_TRUE(keyboard_.up);
  EXPECT_EQ(context_->GetFocusElement(), draft_);
}

TEST_F(ContextMenuKeyboardTest, EditFromTheSheetKeepsTheKeyboardUpThroughout) {
  StartTyping();
  PressAndHoldMessage();
  Lift();
  Frame();
  ui::Element* edit = SheetRow("Edit");
  ASSERT_NE(edit, nullptr);

  Tap(edit);
  for (int i = 0; i < 5; ++i) {
    Frame();
  }
  EXPECT_EQ(edits_, 1);
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(keyboard_.deactivations, 0) << "the keyboard went down and came back up";
  EXPECT_TRUE(keyboard_.up);
  EXPECT_EQ(context_->GetFocusElement(), draft_);
}

TEST_F(ContextMenuKeyboardTest, AnotherRowOrClosingTheSheetLeavesTheKeyboardAsItWas) {
  StartTyping();
  PressAndHoldMessage();
  Lift();
  Frame();
  ui::Element* copy = SheetRow("Copy");
  ASSERT_NE(copy, nullptr);
  Tap(copy);
  Frame();
  EXPECT_EQ(copies_, 1);
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());

  PressAndHoldMessage();
  Lift();
  Frame();
  ui::Element* scrim = document_->GetElementById("context-menu-scrim");
  ASSERT_NE(scrim, nullptr);
  Tap(scrim);
  Frame();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());

  EXPECT_EQ(keyboard_.deactivations, 0);
  EXPECT_TRUE(keyboard_.up);
  EXPECT_EQ(context_->GetFocusElement(), draft_);
}

TEST_F(ContextMenuKeyboardTest, ATapOnAMessageWhileTypingStillPutsTheKeyboardAway) {
  StartTyping();
  context_->ProcessTouchStart(ui::TouchList{touch_}, 0);
  Frame();
  Lift();
  Frame();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(keyboard_.deactivations, 1);
  EXPECT_NE(context_->GetFocusElement(), draft_);
}

TEST_F(ContextMenuKeyboardTest, AfterTheSheetClosesATapOnTheMessageStillPutsTheKeyboardAway) {
  StartTyping();
  PressAndHoldMessage();
  Lift();
  Frame();
  Tap(document_->GetElementById("context-menu-scrim"));
  Frame();
  ASSERT_FALSE(ContextMenuHost::Instance().IsOpen());
  ASSERT_EQ(keyboard_.deactivations, 0);

  context_->ProcessTouchStart(ui::TouchList{touch_}, 0);
  Frame();
  Lift();
  Frame();
  EXPECT_EQ(keyboard_.deactivations, 1);
  EXPECT_NE(context_->GetFocusElement(), draft_);
}

TEST_F(ContextMenuKeyboardTest, ALongPressWithoutTheKeyboardOpensTheSheetAsBefore) {
  ASSERT_FALSE(keyboard_.up);
  PressAndHoldMessage();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(PanelBottom(document_), "0px");
  Lift();
  Frame();
  EXPECT_TRUE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(PanelBottom(document_), "0px");
  EXPECT_EQ(keyboard_.activations, 0);
  EXPECT_EQ(keyboard_.deactivations, 0);
  EXPECT_EQ(msg_->GetLocalProperty("focus"), nullptr);
}

// The composer's own sheets (the plus menu and the like) open from ShowActions, not from a long press: nothing about
// them changes.
TEST_F(ContextMenuKeyboardTest, ASheetOpenedFromAButtonWhileTypingIsUnchanged) {
  StartTyping();
  ContextMenuHost::Instance().ShowActions({10, 600}, OneAction());
  Frame();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(PanelBottom(document_), "300px");
  EXPECT_EQ(msg_->GetLocalProperty("focus"), nullptr);
  Tap(document_->GetElementById("context-menu-scrim"));
  Frame();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(keyboard_.deactivations, 0);
  EXPECT_EQ(context_->GetFocusElement(), draft_);
}

// The desktop's pointer menu (right click) is not a touch: it holds no focus back.
TEST_F(ContextMenuKeyboardTest, ADesktopRightClickMenuHoldsNoFocus) {
  ContextMenuHost::Instance().SetCompactLayout(false);
  ASSERT_TRUE(draft_->Focus());
  Frame();
  ASSERT_TRUE(ContextMenuHost::Instance().OnContextPointer(context_, 50, 60));
  Frame();
  ASSERT_TRUE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(msg_->GetLocalProperty("focus"), nullptr);
}

// Kenneth 2026-11-12: a call record row in a chat opens no menu; one tap on it calls back. The long press on it must
// leave things as they were: no sheet, no click when the finger lifts (that would call back), and a keyboard that is
// up stays up.
namespace {
class ClickCounter : public ui::EventListener {
public:
  void ProcessEvent(ui::Event&) override { ++clicks; }
  int clicks = 0;
};
} // namespace

TEST_F(ContextMenuKeyboardTest, AnEmptyMenuLongPressShowsNothingAndItsLiftIsNoTap) {
  message_actions_ = [](const ContextMenuRequest&) { return std::vector<ContextMenuAction>{}; };
  ClickCounter counter;
  msg_->AddEventListener(ui::EventId::Click, &counter);
  PressAndHoldMessage();
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(document_->GetElementById("context-menu-layer"), nullptr);
  Lift();
  for (int i = 0; i < 3; ++i) {
    Frame();
  }
  EXPECT_EQ(counter.clicks, 0) << "the lift after a long press called back";
  // The next plain tap is a tap again.
  context_->ProcessTouchStart(ui::TouchList{touch_}, 0);
  Frame();
  Lift();
  Frame();
  EXPECT_EQ(counter.clicks, 1);
  msg_->RemoveEventListener(ui::EventId::Click, &counter);
}

TEST_F(ContextMenuKeyboardTest, AnEmptyMenuLongPressWhileTypingLeavesTheKeyboardAndFocusAsTheyWere) {
  message_actions_ = [](const ContextMenuRequest&) { return std::vector<ContextMenuAction>{}; };
  StartTyping();
  PressAndHoldMessage();
  Lift();
  for (int i = 0; i < 3; ++i) {
    Frame();
  }
  EXPECT_FALSE(ContextMenuHost::Instance().IsOpen());
  EXPECT_EQ(keyboard_.deactivations, 0);
  EXPECT_TRUE(keyboard_.up);
  EXPECT_EQ(context_->GetFocusElement(), draft_);
  EXPECT_EQ(msg_->GetLocalProperty("focus"), nullptr);
  // No residue: a tap on the message still puts the keyboard away.
  context_->ProcessTouchStart(ui::TouchList{touch_}, 0);
  Frame();
  Lift();
  Frame();
  EXPECT_EQ(keyboard_.deactivations, 1);
  EXPECT_NE(context_->GetFocusElement(), draft_);
}

} // namespace pbr
