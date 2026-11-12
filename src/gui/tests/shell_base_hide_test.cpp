// DEV/PLAN-call-frame-cost.md step 4: the layers a full-screen phone call page covers are hidden while it is full screen
// (phones only); the layers above the call page are never touched; focus, drafts and scroll positions survive.
#include "gui/shell/ShellBaseHide.h"
#include "gui/shell/ShellLayoutRml.h"
#include "lib/ui/ShellTypes.h"

#include <ui/core/Core.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>
#include <ui/paint/RenderInterface.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace pbr {
namespace {

class NullRender : public ui::RenderInterface {
public:
  ui::CompiledGeometryHandle CompileGeometry(ui::Span<const ui::Vertex>, ui::Span<const int>) override { return 1; }
  void RenderGeometry(ui::CompiledGeometryHandle, ui::Vector2f, ui::TextureHandle) override {}
  void ReleaseGeometry(ui::CompiledGeometryHandle) override {}
  ui::TextureHandle LoadTexture(ui::Vector2i& dimensions, const ui::String&) override {
    dimensions = {64, 64};
    return 2;
  }
  ui::TextureHandle GenerateTexture(ui::Span<const ui::byte>, ui::Vector2i) override { return 1; }
  void ReleaseTexture(ui::TextureHandle) override {}
  void EnableScissorRegion(bool) override {}
  void SetScissorRegion(ui::Rectanglei) override {}
};

ShellState PhoneStateWithCall(const CallChromeMode mode, const bool active = true) {
  ShellState state;
  state.layout_mode = LayoutMode::Compact;
  state.call_in_progress.active = active;
  state.call_in_progress.mode = mode;
  PaneState page;
  page.spec.key = "contact_detail";
  page.id = 1;
  state.transient_stack.push_back(page); // a page pushed over the tab: the transient layer exists
  state.overlay_stack.push_back({1, OverlayKind::Generic, "dialog", "views/dialog.rml", {}});
  return state;
}

TEST(ShellBaseHideTest, HidesOnlyWhileThePhoneCallPageIsFullScreen) {
  EXPECT_TRUE(ShellBaseHide::ShouldHide(PhoneStateWithCall(CallChromeMode::Expanded)));
  EXPECT_TRUE(ShellBaseHide::ShouldHide(PhoneStateWithCall(CallChromeMode::Immersive)));
  EXPECT_FALSE(ShellBaseHide::ShouldHide(PhoneStateWithCall(CallChromeMode::Minimized))) << "small window";
  EXPECT_FALSE(ShellBaseHide::ShouldHide(PhoneStateWithCall(CallChromeMode::Expanded, false))) << "call ended";
  EXPECT_FALSE(ShellBaseHide::ShouldHide(ShellState{})) << "call state cleared";
  ShellState desktop = PhoneStateWithCall(CallChromeMode::Expanded);
  desktop.layout_mode = LayoutMode::Expanded;
  EXPECT_FALSE(ShellBaseHide::ShouldHide(desktop)) << "desktop / tablet: the call bar covers nothing";
}

// DEV/PLAN-call-failed-redial.md 6: the failed page kept with no call behind it covers the screen as the call page did,
// and the pages under it show as soon as it is closed.
TEST(ShellBaseHideTest, TheKeptFailedPageHidesWhatItCoversUntilItCloses) {
  ShellState kept = PhoneStateWithCall(CallChromeMode::Expanded);
  kept.call_in_progress.phone_failed = true;
  kept.call_in_progress.failed_kept = true;
  EXPECT_TRUE(ShellBaseHide::ShouldHide(kept));
  ShellState closed = kept;
  closed.call_in_progress = {};
  EXPECT_FALSE(ShellBaseHide::ShouldHide(closed));
}

TEST(ShellBaseHideTest, WhatShowsAboveTheCallPageDoesNotChangeIt) {
  ShellState ring_only;
  ring_only.layout_mode = LayoutMode::Compact;
  ring_only.call_ring.active = true;
  EXPECT_FALSE(ShellBaseHide::ShouldHide(ring_only)) << "an incoming call page alone: no call page under it";
  ShellState ring_over_call = PhoneStateWithCall(CallChromeMode::Expanded);
  ring_over_call.call_ring.active = true;
  EXPECT_TRUE(ShellBaseHide::ShouldHide(ring_over_call)) << "a second call ringing over the call page";
  ShellState ring_over_chip = PhoneStateWithCall(CallChromeMode::Minimized);
  ring_over_chip.call_ring.active = true;
  EXPECT_FALSE(ShellBaseHide::ShouldHide(ring_over_chip));
  ShellState pin_over_call = PhoneStateWithCall(CallChromeMode::Expanded);
  pin_over_call.pin_gate.active = true;
  EXPECT_TRUE(ShellBaseHide::ShouldHide(pin_over_call)) << "the PIN page sits above the call page";
  ShellState dialog_over_call = PhoneStateWithCall(CallChromeMode::Immersive);
  dialog_over_call.dialog.active = true;
  EXPECT_TRUE(ShellBaseHide::ShouldHide(dialog_over_call)) << "a dialog / choice page over the call (overlay layer)";
}

class ShellBaseHideDomTest : public testing::Test {
protected:
  void SetUp() override {
    ui::SetRenderInterface(&renderer);
    initialized = ui::Initialise();
    ASSERT_TRUE(initialized);
    context = ui::CreateContext("shell-base-hide-test", {390, 844});
    ASSERT_NE(context, nullptr);
    ShellState state = PhoneStateWithCall(CallChromeMode::Expanded);
    std::string root = ShellLayoutRml(state, false, "").ShellRoot();
    root = std::regex_replace(root, std::regex("\\s+data-[a-z-]+=\"[^\"]*\""), ""); // no data models here
    const std::string rml =
        "<rml><head><style>body, div { display: block; } input { display: inline-block; width: 200px; height: 20px; }"
        "#shell-root { position: absolute; top: 0; left: 0; width: 390px; height: 844px; }"
        ".shell-layer { position: absolute; top: 0; left: 0; right: 0; bottom: 0; }</style></head><body>"
        "<div id=\"shell-root\">" +
        root + "</div></body></rml>";
    document = context->LoadDocumentFromMemory(rml);
    ASSERT_NE(document, nullptr);
    document->Show();
    context->Update();
  }
  void TearDown() override {
    if (context) {
      ui::RemoveContext("shell-base-hide-test");
    }
    if (initialized) {
      ui::Shutdown();
    }
    ui::SetRenderInterface(nullptr);
  }
  bool Shown(const char* id) {
    ui::Element* element = document->GetElementById(id);
    EXPECT_NE(element, nullptr) << id;
    return element && element->IsVisible(true);
  }
  NullRender renderer;
  bool initialized = false;
  ui::Context* context = nullptr;
  ui::ElementDocument* document = nullptr;
};

TEST_F(ShellBaseHideDomTest, EveryCoveredLayerHidesAndComesBack) {
  // By name: the base layer (tab / chat pages and the tab bar), the AI drawer host, the pushed-page layer.
  ASSERT_EQ(std::size(ShellBaseHide::kLayerIds), 3u);
  for (const char* id : ShellBaseHide::kLayerIds) {
    EXPECT_TRUE(Shown(id)) << id << " before";
  }
  ShellBaseHide::Apply(context, document, true);
  context->Update();
  EXPECT_TRUE(ShellBaseHide::Hidden());
  EXPECT_FALSE(Shown("shell-base-layer"));
  EXPECT_FALSE(Shown("shell-ai-drawer-host"));
  EXPECT_FALSE(Shown("shell-transient-layer"));
  ShellBaseHide::Apply(context, document, false);
  context->Update();
  EXPECT_FALSE(ShellBaseHide::Hidden());
  for (const char* id : ShellBaseHide::kLayerIds) {
    EXPECT_TRUE(Shown(id)) << id << " after";
  }
}

TEST_F(ShellBaseHideDomTest, TheLayersAboveTheCallPageAreNotTouched) {
  ShellBaseHide::Apply(context, document, true);
  context->Update();
  // The overlay layer (dialogs, pages and sheets over the call), the call mounts, the dialog / photo / PIN mounts.
  ui::ElementList overlays;
  document->GetElementsByClassName(overlays, "shell-layer-overlay");
  ASSERT_FALSE(overlays.empty());
  for (ui::Element* overlay : overlays) {
    EXPECT_TRUE(overlay->IsVisible(true)) << "overlay layer";
  }
  for (const char* id : {"shell-call-in-progress-mount", "shell-call-ring-mount", "shell-dialog-mount",
                         "shell-photo-mount", "shell-pin-gate-mount", "shell-onboarding-mount"}) {
    EXPECT_TRUE(Shown(id)) << id;
  }
  ShellBaseHide::Apply(context, document, false);
}

// A chat in the base layer: a long transcript scrolled to the middle and a composer holding a draft, with the focus.
struct ChatProbe {
  ui::Element* transcript = nullptr;
  ui::Element* composer = nullptr;
};

ChatProbe AddChat(ui::ElementDocument* document) {
  ChatProbe chat;
  ui::Element* base = document->GetElementById("shell-base-layer");
  if (!base) {
    return chat;
  }
  ui::ElementPtr transcript = document->CreateElement("div");
  transcript->SetId("probe-transcript");
  transcript->SetAttribute("style", "position: absolute; top: 0; left: 0; width: 390px; height: 600px; overflow-y: auto;");
  transcript->SetInnerRML("<div style=\"height: 3000px;\"></div>");
  chat.transcript = base->AppendChild(std::move(transcript));
  ui::ElementPtr composer = document->CreateElement("input");
  composer->SetId("probe-composer");
  composer->SetAttribute("type", "text");
  composer->SetAttribute("style", "position: absolute; top: 700px; left: 0;");
  chat.composer = base->AppendChild(std::move(composer));
  return chat;
}

TEST_F(ShellBaseHideDomTest, TheFocusLeavesTheLayersAsTheyHide) {
  ChatProbe chat = AddChat(document);
  ASSERT_NE(chat.composer, nullptr);
  context->Update();
  ASSERT_TRUE(chat.composer->Focus());
  ASSERT_EQ(context->GetFocusElement(), chat.composer);
  ShellBaseHide::Apply(context, document, true);
  context->Update();
  ui::Element* focus = context->GetFocusElement();
  EXPECT_NE(focus, chat.composer) << "the composer (and the keyboard) let go";
  EXPECT_FALSE(ShellBaseHide::InHiddenLayer(document, focus)) << "the focus is on nothing hidden";
  EXPECT_TRUE(ShellBaseHide::InHiddenLayer(document, chat.composer)) << "so a later focus restore skips it";
  ShellBaseHide::Apply(context, document, false);
  EXPECT_FALSE(ShellBaseHide::InHiddenLayer(document, chat.composer));
  EXPECT_NE(context->GetFocusElement(), chat.composer) << "the focus is not forced back";
}

TEST_F(ShellBaseHideDomTest, TheDraftAndTheScrollPositionSurvive) {
  ChatProbe chat = AddChat(document);
  ASSERT_NE(chat.transcript, nullptr);
  context->Update();
  chat.composer->SetAttribute("value", "half a sentence");
  chat.transcript->SetScrollTop(1200.f); // the middle: neither the top nor the bottom
  context->Update();
  ASSERT_FLOAT_EQ(chat.transcript->GetScrollTop(), 1200.f);
  ShellBaseHide::Apply(context, document, true);
  context->Update();
  ShellBaseHide::Apply(context, document, false);
  context->Update();
  EXPECT_EQ(document->GetElementById("probe-transcript"), chat.transcript) << "not remounted";
  EXPECT_FLOAT_EQ(chat.transcript->GetScrollTop(), 1200.f);
  EXPECT_EQ(chat.composer->GetAttribute<ui::String>("value", ""), "half a sentence");
}

} // namespace
} // namespace pbr
