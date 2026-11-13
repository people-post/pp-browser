#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellHost.h"
#include "gui/shell/ShellLargeTitleScroll.h"
#include "lib/ui/ShellInterruption.h"
#include "lib/ui/ShellLayout.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
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
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 0; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

class LargeTitleTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("large-title-test", {400, 800});
    ASSERT_NE(context, nullptr);
  }
  void TearDown() override {
    DataModelHost::Instance().Clear();
    if (context) {
      ui::RemoveContext("large-title-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
};

TEST(LargeTitleRules, CollapsesOnceTheWholeTitleIsScrolledAway) {
  EXPECT_FALSE(ShellLargeTitleScroll::CollapsedAt(0.f, 1.f));
  EXPECT_FALSE(ShellLargeTitleScroll::CollapsedAt(51.f, 1.f));
  EXPECT_TRUE(ShellLargeTitleScroll::CollapsedAt(52.f, 1.f));
  EXPECT_FALSE(ShellLargeTitleScroll::CollapsedAt(103.f, 2.f)) << "px are converted with the dp ratio";
  EXPECT_TRUE(ShellLargeTitleScroll::CollapsedAt(104.f, 2.f));
  EXPECT_FALSE(ShellState{}.large_title_collapsed);
}

// A scroller may carry `collapse-at` (dp) to replace the 52dp default threshold; the Chats page sets 72dp (its row holds the 56dp avatar block).
TEST(LargeTitleRules, ThresholdCanBeOverriddenPerScroller) {
  EXPECT_FALSE(ShellLargeTitleScroll::CollapsedAt(19.f, 1.f, 20.f));
  EXPECT_TRUE(ShellLargeTitleScroll::CollapsedAt(20.f, 1.f, 20.f));
  EXPECT_TRUE(ShellLargeTitleScroll::CollapsedAt(40.f, 2.f, 20.f)) << "px are converted with the dp ratio";
  const std::string rml = [] {
    std::ifstream in(std::filesystem::path(PP_BROWSER_ASSETS_DIR) / "views" / "sidebar.rml");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }();
  EXPECT_NE(rml.find("collapse-at=\"72\""), std::string::npos) << "Chats collapses once its 72dp row has scrolled away";
  EXPECT_NE(rml.find("<div class=\"large-title\">"), std::string::npos) << "Chats has a large title row to collapse";
}

TEST_F(LargeTitleTest, ValueChangesAndTheNavPageFollows) {
  ShellHost host;
  ASSERT_TRUE(host.RegisterWindowModel(context));
  auto* document = context->LoadDocumentFromMemory(
      "<rml><body><div id=\"page\" data-model=\"window\" "
      "data-class-large-title-collapsed=\"large_title_collapsed\"></div></body></rml>");
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  ui::Element* page = document->GetElementById("page");
  ASSERT_NE(page, nullptr);
  EXPECT_FALSE(page->IsClassSet("large-title-collapsed"));

  host.SetLargeTitleCollapsed(true);
  context->Update();
  EXPECT_TRUE(page->IsClassSet("large-title-collapsed"));
  EXPECT_TRUE(host.State().large_title_collapsed);

  host.SetLargeTitleCollapsed(false);
  context->Update();
  EXPECT_FALSE(page->IsClassSet("large-title-collapsed"));
}

// Settings' back arrow is bound to compact_chat_open: whichever way the chat thread opens or closes, the value the view
// sees is the actual one (no separate field to forget to dirty).
TEST_F(LargeTitleTest, TheSettingsBackArrowSeesTheChatThreadOnEveryPath) {
  ShellHost host;
  ASSERT_TRUE(host.RegisterWindowModel(context));
  host.State().layout_mode = LayoutMode::Compact;
  ShellLayout::SyncLayoutModeString(host.State());
  host.RegisterPane({.key = "chat", .rml_path = "views/chat.rml", .provides_composer = true});
  auto* document = context->LoadDocumentFromMemory(
      "<rml><body><div data-model=\"window\"><p id=\"back\" style=\"width: 5px; height: 5px;\" "
      "data-if=\"compact_chat_open\"></p></div></body></rml>");
  ASSERT_NE(document, nullptr);
  document->Show();
  // Every one of these changes asks for a layout sync, which dirties the whole window model (DirtyWindow); the sync
  // needs the full shell DOM, so the test stands in for it with DirtyNavChrome, which covers compact_chat_open.
  const auto shown = [&]() {
    host.DirtyNavChrome();
    context->Update();
    return document->GetElementById("back")->IsVisible();
  };
  const auto agrees = [&](const char* path) {
    EXPECT_EQ(shown(), host.State().compact_chat_open) << path << ": the view and the state agree";
  };
  host.SelectNavTab(NavTab::Sessions);
  agrees("start");
  EXPECT_FALSE(shown());
  host.SetPrimaryPane("chat");
  host.OpenCompactChat();
  host.SelectNavTab(NavTab::Me); // a hint in the chat opens Settings over it
  EXPECT_TRUE(shown()) << "OpenCompactChat + SelectNavTab(Me)";
  EXPECT_TRUE(ShellLayout::HiddenTabHasBack(host.State()));
  host.HiddenTabBack(); // the arrow: the chat is shown again, no Settings page
  agrees("back to the thread");
  host.CloseCompactChat();
  EXPECT_FALSE(shown()) << "CloseCompactChat";
  host.SelectNavTab(NavTab::Me);
  EXPECT_FALSE(shown()) << "Settings with no chat under it: no arrow";
  EXPECT_FALSE(ShellLayout::HiddenTabHasBack(host.State()));
  host.SelectNavTab(NavTab::Home); // ClearTabContext
  host.OpenCompactChat();
  host.SelectNavTab(NavTab::Me);
  EXPECT_TRUE(shown());
  host.SelectNavTab(NavTab::Sessions);
  host.SelectNavTab(NavTab::Home); // leaving drops the chat (ClearTabContext)
  EXPECT_FALSE(shown()) << "SelectNavTab to another tab closes the chat";
  // ShellInterruption::DismissTop closes the chat directly in the state (ShellInterruption.cpp); no production caller
  // goes through it without a layout sync, which dirties compact_chat_open (DirtyNavChrome).
  host.OpenCompactChat();
  ShellInterruption::DismissTop(host.State());
  host.DirtyNavChrome();
  EXPECT_FALSE(shown()) << "the interruption path, then the shell's sync";
}

// The Chats header's status line (SetChatsLink): unknown shows nothing, then online / offline come through the window
// model as state + text; the same value again changes nothing; going back to unknown clears the line.
TEST_F(LargeTitleTest, ChatsLinkStatusFollowsSetChatsLink) {
  ShellHost host;
  ASSERT_TRUE(host.RegisterWindowModel(context));
  auto* document = context->LoadDocumentFromMemory(
      "<rml><body><div id=\"page\" data-model=\"window\">"
      "<p id=\"line\" data-if=\"chats_link_state != ''\">{{chats_link_text}}</p>"
      "<span id=\"ok\" style=\"width: 5px; height: 5px;\" data-if=\"chats_link_state == 'ok'\"></span>"
      "<span id=\"off\" style=\"width: 5px; height: 5px;\" data-if=\"chats_link_state == "
      "'offline'\"></span></div></body></rml>");
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  EXPECT_FALSE(document->GetElementById("line")->IsVisible()) << "unknown state: nothing is drawn";
  EXPECT_TRUE(host.State().chats_link_state.empty());

  host.SetChatsLink(false, true);
  context->Update();
  EXPECT_TRUE(host.State().chats_link_state.empty()) << "known=false ignores the flag";

  host.SetChatsLink(true, false);
  context->Update();
  EXPECT_EQ(host.State().chats_link_state, "offline");
  EXPECT_FALSE(host.State().chats_link_text.empty());
  EXPECT_TRUE(document->GetElementById("line")->IsVisible());
  EXPECT_TRUE(document->GetElementById("off")->IsVisible());
  EXPECT_FALSE(document->GetElementById("ok")->IsVisible());
  const auto offline_text = host.State().chats_link_text;

  host.SetChatsLink(true, false);
  context->Update();
  EXPECT_EQ(host.State().chats_link_text, offline_text) << "the same value changes nothing";

  host.SetChatsLink(true, true);
  context->Update();
  EXPECT_EQ(host.State().chats_link_state, "ok");
  EXPECT_NE(host.State().chats_link_text, offline_text);
  EXPECT_TRUE(document->GetElementById("ok")->IsVisible());
  EXPECT_FALSE(document->GetElementById("off")->IsVisible());

  host.SetChatsLink(false, false);
  context->Update();
  EXPECT_TRUE(host.State().chats_link_state.empty());
  EXPECT_TRUE(host.State().chats_link_text.empty());
  EXPECT_FALSE(document->GetElementById("line")->IsVisible());
}

TEST_F(LargeTitleTest, ScrollingTheRootScrollerReportsOnlyWhenTheStateFlips) {
  auto* document = context->LoadDocumentFromMemory(
      "<rml><head><style>body{width:400px;height:800px;} #page{width:400px;height:300px;} "
      "#scroller{width:400px;height:300px;overflow-y:auto;} .row{height:100px;}</style></head><body>"
      "<div id=\"page\"><div id=\"scroller\" class=\"root-scroll\">"
      "<div class=\"row\"></div><div class=\"row\"></div><div class=\"row\"></div><div class=\"row\"></div>"
      "</div><div id=\"other\" style=\"overflow-y:auto;height:50px\"><div style=\"height:500px\"></div></div>"
      "</div></body></rml>");
  ASSERT_NE(document, nullptr);
  document->Show();
  context->Update();
  ui::Element* page = document->GetElementById("page");
  ui::Element* scroller = document->GetElementById("scroller");
  ui::Element* other = document->GetElementById("other");
  ASSERT_TRUE(page && scroller && other);

  ShellLargeTitleScroll tracker;
  std::vector<bool> reports;
  tracker.Attach(page, [&reports](bool collapsed) { reports.push_back(collapsed); });
  EXPECT_TRUE(reports.empty()) << "starts expanded";

  const float ratio = context->GetDensityIndependentPixelRatio();
  scroller->SetScrollTop(10.f);
  scroller->SetScrollTop(20.f);
  EXPECT_TRUE(reports.empty()) << "no report while the state is unchanged";
  scroller->SetScrollTop(60.f * ratio);
  ASSERT_EQ(reports.size(), 1u);
  EXPECT_TRUE(reports.back());
  scroller->SetScrollTop(70.f * ratio);
  EXPECT_EQ(reports.size(), 1u);
  other->SetScrollTop(200.f);
  EXPECT_EQ(reports.size(), 1u) << "other scrollers are ignored";
  scroller->SetScrollTop(0.f);
  ASSERT_EQ(reports.size(), 2u);
  EXPECT_FALSE(reports.back());
  tracker.Detach();
}

} // namespace
} // namespace pbr
