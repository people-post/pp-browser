#include "gui/shell/DataModelHost.h"
#include "gui/shell/ShellHost.h"
#include "gui/shell/ShellLargeTitleScroll.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

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
    if (context) { ui::RemoveContext("large-title-test"); }
    if (initialized) { ui::Shutdown(); }
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
